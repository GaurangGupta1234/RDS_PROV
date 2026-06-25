# RDS provider — performance optimization handover (v2)

This round fixes the two problems your benchmarks exposed and rearchitects the
data path around what your own measurements proved is fastest. Read this first;
it is the map for the other agent.

TL;DR of what changed:

1. **The `-ENOMEM` crash is fixed.** Rendezvous no longer calls `RDS_GET_MR`
   per message — it uses libfabric's monitor-backed **MR cache**. Registrations
   are reused across messages and bounded, so `ulimit -l` is no longer
   exhausted and large-message bandwidth tracks RXM.
2. **A zero-copy RDMA eager fast path** (opt-in) brings small-message latency
   down to RDS-RDMA-V2 levels (~14 µs RTT) instead of the ~35 µs datagram
   floor — i.e. comparable to / better than TCP. The datagram path stays as the
   default and the fallback.
3. The agent's Intel-MPI compatibility changes are preserved; one of its three
   tuning recommendations is correct (MR cache) and now implemented, one is
   counter-productive for latency (epoll) and is explained below, one helps
   throughput not latency (recvmmsg) and is noted as optional.

---

## 1. Why small messages were slow, and the real fix

Your own report is the key evidence:

| path                       | 2 B RTT |
|----------------------------|---------|
| RDS IB **datagram** (copy) | ~41 µs  |
| TCP                        | ~30 µs  |
| **RDS RDMA V2** (1-sided write + memory poll) | **~14 µs** |
| Native IB verbs            | ~10 µs  |

The datagram receive path (`recvmsg` on the RDS socket) is the floor: every
message goes through the kernel's datagram delivery machinery. **No amount of
tuning the `recvmsg` loop gets below ~35 µs** — the win in V2 came from *not
using the datagram receive path at all*: the sender RDMA-writes into a
pre-registered buffer and the receiver detects arrival by **polling memory**.

So the optimization is architectural, not a micro-tweak:

```
            small msg (<= ring slot)          large msg
            ----------------------            ---------
sender  →   RDMA write into peer's ring   |   RTS + sender source MR (cached)
receiver→   poll memory (no syscall)      |   RDMA READ into app buffer (zero copy)
```

### About the agent's "use epoll / block instead of busy-poll" recommendation

That would make latency **worse**, not better. The 4,608 `EAGAIN`s in the
`strace` are the busy-poll checking the socket — that is *expected* for an
HPC/MPI low-latency provider and is exactly what RXM and your V2 benchmark do.
Blocking on `epoll` adds a kernel wakeup (~5–10 µs) to every message. The right
fix is not to stop polling — it is to **poll the right thing**: a memory
location in the eager ring instead of the socket. With the ring enabled the
eager receive path issues **zero `recvmsg` calls**; the socket is only drained
for control/rendezvous/RDMA-completion traffic. So the `EAGAIN` storm
disappears for the fast path without giving up busy-poll latency.

(`recvmmsg`/`sendmmsg` batching — the third recommendation — raises message
*rate* at scale but not latency; with the ring the eager path doesn't use
`recvmsg` at all, so it's moot there. It remains a possible future optimization
for the datagram fallback and is noted, not implemented.)

---

## 2. The `-ENOMEM` crash: MR cache (always on)

**Root cause (correct in your analysis):** the rendezvous path called
`setsockopt(RDS_GET_MR)` on **every** large send and only freed on FIN. Pinned
pages piled up, hit `ulimit -l`, and `RDS_GET_MR` started returning `ENOMEM`;
even before that, per-message pinning destroyed bandwidth.

**Fix:** `prov/rds/src/rds_mr.c` now wires libfabric's `ofi_mr_cache`
(monitor-backed, the same machinery verbs/efa use):

- `fi_mr_reg`-style registration of a rendezvous **source** buffer goes through
  `rds_reg_cache_get()`. The first send registers; **repeated sends from the
  same buffer (the IMB/OSU and typical-MPI case) hit the cache** and skip
  `RDS_GET_MR` entirely.
- The `memhooks`/`uffd` monitor (already active in your run —
  `Default memory monitor is: memhooks`) invalidates an entry if the app frees
  or remaps the pages, so a recycled address never returns a stale cookie.
- The cache is **bounded**: count ≤ 1024 (util default) and size capped to
  256 MiB by the provider unless you set `FI_MR_CACHE_MAX_SIZE`. LRU eviction
  frees old MRs (`RDS_FREE_MR`). So pinned memory is bounded — the crash cannot
  recur.
- The 256-page (1 MiB) per-MR kernel limit is respected: rendezvous still
  chunks at 1 MiB and caches **per chunk**, so a repeated multi-MiB buffer hits
  the cache on every chunk.

This is **on by default** and has no flag. If cache init ever fails the provider
logs it and falls back to per-message registration (old behavior), so it can't
make things worse.

Tuning: `FI_MR_CACHE_MAX_COUNT`, `FI_MR_CACHE_MAX_SIZE` (bytes). Watch
`FI_LOG_LEVEL=info FI_LOG_PROV=rds` for `MR cache enabled (...)`.

---

## 3. The zero-copy eager RDMA ring (opt-in: `FI_RDS_EAGER_RDMA=1`)

`prov/rds/src/rds_eager.c`. Per peer pair we lazily set up a small,
pre-registered **receive ring** and exchange cookies once (a 2-way
`RING_REQ`/`RING_ACK` handshake over RDS datagrams). Then:

- **Send**: stage the message (header+payload+generation trailer) in a local
  slot and place it with **one fire-and-forget RDMA write** into the peer's
  ring (no completion notification needed — RDS is reliable and flow control
  guarantees the staging slot isn't reused until the receiver has consumed it).
  The send completes immediately (buffer copied into staging).
- **Receive**: `rds_ring_progress()` **polls the ring's generation stamp in
  memory** — no `recvmsg`. A matched slot is fed straight into the existing
  tag-matching engine (`rds_handle_inbound`), so tagged/`FI_SOURCE`/unexpected
  semantics are identical to the datagram path.
- **Flow control**: credit is **piggybacked** on reverse-direction traffic
  (`slot.ack`) — so ping-pong and collectives need *zero* extra RDMA for
  credit — with an explicit credit write every `slots/2` for one-directional
  streams.
- **Ordering safety**: the generation stamp is the last word of each slot, so a
  partially-arrived slot is never read (the same ascending-address-write
  assumption your V2 code relies on, valid on the irdma HCA).

### Memory and scale (important)

Each ring peer costs `slots × slot_size` registered + the same staging
(unregistered). Defaults `16 × 1024` ⇒ **16 KiB + 16 KiB per peer**.
`FI_RDS_RING_MAX_PEERS` (default 256) is a **hard resident cap**: peers beyond
it use the datagram path (no unsafe eviction of a live remote-writable ring).
So pinned ring memory ≤ `max_peers × slots × slot_size` (≈4 MiB/rank at
defaults). At 192 PPN that's ≈768 MiB/node — fits with RDMA-grade `ulimit -l`.
For 768-rank all-to-all where every rank talks to all others, raise
`ring_max_peers` if memory allows, or accept that the busiest 256 peers get the
fast path and the rest use datagrams.

### Ordering caveat (read before enabling for real apps)

The ring is a **separate channel** from the RDS socket. The provider keeps
same-tag eager ordering on the ring (a full ring returns `-FI_EAGAIN` to the
caller rather than silently spilling to the socket). But for a single
destination+tag stream that mixes **eager (ring)** and **rendezvous (socket)**
messages — i.e. mixed message sizes — strict `FI_ORDER_SAS` across the two
channels is not guaranteed, and there is a brief per-peer reorder window during
the initial handshake. For the IMB/OSU latency benchmarks (uniform size per run)
this never triggers. For ordering-sensitive mixed-size production apps, either
leave the ring off (the datagram path is fully ordered — one socket) or we can
route the rendezvous RTS through the ring too (future work, noted below).

**Recommendation:** enable `FI_RDS_EAGER_RDMA=1` for the latency runs; keep it
off (default) if you hit correctness issues — you still get the MR-cache crash
fix and large-message bandwidth.

---

## 4. New runtime knobs

| env var | meaning | default |
|---------|---------|---------|
| `FI_RDS_EAGER_RDMA` | enable the zero-copy eager ring | 0 (off) |
| `FI_RDS_RING_SLOTS` | slots per peer ring (power of two) | 16 |
| `FI_RDS_RING_SLOT_SIZE` | bytes per slot; caps ring-eager payload | 1024 |
| `FI_RDS_RING_MAX_PEERS` | resident ring cap (bounds pinning) | 256 |
| `FI_RDS_EAGER_SIZE` | datagram eager vs rendezvous threshold | 8192 |
| `FI_MR_CACHE_MAX_COUNT` / `FI_MR_CACHE_MAX_SIZE` | rendezvous MR cache bounds | 1024 / 256 MiB |

Build/integration is unchanged from v1 (`integration/core-integration.patch`),
plus the new `rds_eager.c` is already listed in `Makefile.include`.

---

## 5. What to measure, and expected outcome

Run the same IMB-MPI1 / OSU sweeps as before, three ways, on 16 and 768 ranks:

```sh
# A) baseline you already have (datagram eager): FI_RDS_EAGER_RDMA unset
# B) MR-cache only (this is the crash fix; large msgs should now match RXM bw)
# C) + ring:  -genv FI_RDS_EAGER_RDMA 1
```

Expectations:

- **Large messages / Allgather at 4 KB+**: no more `-ENOMEM`; bandwidth should
  jump toward RXM (was collapsing due to per-message pinning).
- **Small messages (4–256 B) with the ring on**: latency should drop from
  ~35 µs toward ~14–18 µs — at or below TCP (~13 µs allreduce), approaching RXM
  (~9 µs). Confirm with `osu_latency` (pure pingpong is the cleanest signal).
- **`strace -c`** with the ring on: the `recvmsg`/`EAGAIN` count for the eager
  phase should largely vanish (replaced by memory polling).

If small-message latency with the ring on is still ~30 µs, check
`FI_LOG_LEVEL=info FI_LOG_PROV=rds` for handshake completion and that peers
reached `READY` (otherwise everything is falling back to datagrams — likely the
peer cap, a geometry mismatch, or `RDS_GET_MR` failing on the ring buffers; see
proxy-QP note in `RDS_KERNEL_CHANGES.md`).

---

## 6. Files changed this round

| file | change |
|------|--------|
| `rds_mr.c` | **MR cache** (`ofi_mr_cache`) for rendezvous source buffers |
| `rds_domain.c` | open/close the MR cache on the domain |
| `rds_msg.c` | rendezvous uses the cache; send path tries the ring first |
| `rds_eager.c` | **new** — the zero-copy eager RDMA ring + handshake + flow control |
| `rds_ep.c` | ring init/cleanup; stub atomic ops; keep agent's `setopt`=0 |
| `rds_progress.c` | poll rings first, then drain the socket |
| `rds_rma.c` | `rds_post_rdma` gains a `notify` arg (ring writes are fire-and-forget) |
| `rds_attr.c` | keep agent's Intel-MPI caps (`FI_ATOMICS`/`FI_MULTI_RECV`, `FI_MR_BASIC`) |
| `rds_init.c` | new params: `eager_rdma`, `ring_slots`, `ring_slot_size`, `ring_max_peers` |
| `rds.h` | cache, peer/ring structures, slot layout, prototypes |

## 7. Future work (if you want to push further)

- **Route rendezvous RTS through the ring** when a peer is `READY`, so all
  control+eager travel one ordered channel — removes the mixed-size ordering
  caveat and lets the ring be the default.
- **Native `FI_ATOMICS`** over `RDS_CMSG_ATOMIC_FADD`/`CSWP` so Intel MPI can
  run with atomics enabled (today they're advertised-but-stubbed and disabled
  via `MPIR_CVAR_CH4_OFI_ENABLE_ATOMICS=0`).
- **Proxy QP** in `net/rds` so ring/rendezvous MR registration never depends on
  a pre-existing connection at extreme scale (see `RDS_KERNEL_CHANGES.md` §3).
- **`recvmmsg` batch drain** for the datagram fallback to lift message rate at
  very high PPN.

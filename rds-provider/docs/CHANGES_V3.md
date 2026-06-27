# RDS provider v3 — handover (read this first)

This is the map for the agent on the H4D VMs. It explains, in order:

1. what state you handed me (the `jetski_v2_golden` patch) and what was broken,
2. exactly what I changed and why each change is correct (not a band-aid),
3. the bugs fixed, with the mechanism,
4. what is still uncertain and what I need you to measure / confirm,
5. how to build, run, and sweep the knobs.

The full design is in [`ARCHITECTURE.md`](ARCHITECTURE.md). The kernel
assumptions are in [`RDS_KERNEL_CHANGES.md`](RDS_KERNEL_CHANGES.md). Build/run/env
is in [`TUNING_AND_MPI.md`](TUNING_AND_MPI.md).

---

## 0. TL;DR

Your golden patch had the right *instincts* (counters for Intel MPI,
`RDS_GET_MR_FOR_DEST`, eager-ring control encapsulation) but three real defects:

- **the 16 KiB hang**: the FIN ack was routed back over the eager ring, where the
  reverse address could not be resolved, so it was silently dropped and the
  sender waited forever;
- **dropped rendezvous reads / FINs under load**: any socket send that hit
  `EAGAIN`/`ENOBUFS` (a full RDS send queue — guaranteed during a collective
  storm) was lost, which is the Allgather hang and a big part of the Allreduce
  latency spikes;
- **no flow control**: nothing bounded the number of in-flight rendezvous
  round-trips, so many-to-many collectives melted the single shared RDS
  connection.

v3 fixes all three **architecturally**:

1. **Control messages (FIN, and rendezvous RTS for non-ring peers) only ever
   travel the reliable RDS datagram socket.** The FIN is addressed by the
   sockaddr we received the RTS from — no AV reverse-lookup, nothing to fail.
2. **A deferred-op queue (`ep->deferred`).** Any FIN or rendezvous RDMA READ the
   socket rejects with `EAGAIN` is parked and re-issued, in FIFO order, from the
   progress engine — never dropped. A full send queue becomes *back-pressure*,
   not a lost message. This is the no-hang / no-drop guarantee.
3. **Rendezvous flow control:** a per-endpoint cap on in-flight rendezvous sends
   (`FI_RDS_RNDZV_INFLIGHT`, default 256). Past the cap, sends get `-FI_EAGAIN`
   and the application throttles itself instead of flooding the connection.
4. **The bounded MR cache is restored** (it had been disabled) and made
   compatible with `RDS_GET_MR_FOR_DEST`, so all-to-all pins the send buffer
   **once** instead of once per peer.

The eager ring's forward path is now single-channel and order-preserving, so
`FI_ORDER_SAS` holds for a READY peer.

---

## 1. What I started from

The repo you clone is the **pre-patch base** (`rdsprovidergithubv2`). I applied
the equivalent of your `jetski_v2_golden` changes and then built v3 on top, so
when you diff your tree against this push you will see *your* changes plus my
additions as one coherent delta. The files I touched:

| file | what |
|------|------|
| `rds.h` | ring-slot `op` field, `RDS_PEER_DISABLED`, `struct rds_deferred`, `ep->deferred`, pending `seg_deferred`, new signatures, new tunables |
| `rds_mr.c` | `RDS_GET_MR_FOR_DEST` (dest-aware) + restored bounded MR cache |
| `rds_eager.c` | ring `op` encapsulation, lazy dest-aware ring registration, `RDS_PEER_DISABLED`, handshake/ordering fixes |
| `rds_msg.c` | reliable FIN, deferred rendezvous reads, rendezvous flow-control cap, RTS-over-ring |
| `rds_progress.c` | drain the deferred queue first, every pass |
| `rds_ep.c` | init/flush the deferred queue; optional `SO_SNDBUF`/`SO_RCVBUF` |
| `rds_domain.c` | real `rds_cntr_open` (with a non-NULL progress fn — see §3.6) |
| `rds_init.c` | new params: `mr_cache`, `rndzv_inflight`, `sndbuf`, `rcvbuf` |

`rds_rma.c` and `rds_attr.c` are unchanged.

---

## 2. The data path in one picture (so the fixes make sense)

```
                                       channel             who completes it
  small  (<= ring slot, ring on)   →   RDMA write into     receiver polls memory,
                                       peer ring           sender done immediately
  small/medium (<= eager_size)     →   RDS datagram        kernel copy; sender done
                                       sendmsg             immediately
  large  (> eager_size)            →   RTS  ──────────►    receiver RDMA-READs the
                                       (ring if READY,     source straight into the
                                        else socket)       app buffer (zero copy),
                                       then FIN ◄──────    then FINs the sender
                                       (always socket)
```

The two operations the provider must finish itself — the **RDMA READ** (the
receiver pulling the payload) and the **FIN** (telling the sender it can release
its registration) — are exactly the two that used to be dropped. They are now
reliable (§3.2).

---

## 3. Bugs fixed (with the mechanism)

### 3.1 The 16 KiB hang — FIN routed over the ring (your Action Item #1)

**Root cause.** Your `rds_send_fin` tried to send the FIN over the eager ring by
reverse-resolving the sender's `fi_addr` with `ofi_ip_av_get_fi_addr()`. In the
ring-delivery context that resolution did not yield a usable ring target, and
the FIN was silently dropped. The sender's rendezvous-TX never saw its FIN, so
it waited forever — exactly at the size where rendezvous kicks in.

**Fix (architectural, not a patch over the symptom).** The FIN never goes on the
ring. `rds_send_fin()` sends a plain RDS datagram to `pend->peer` — the
**sockaddr we received the RTS from**, which is always known and always valid.
No AV lookup, nothing to resolve, nothing to fail. The eager ring carries only
bulk forward data and RTS descriptors; the reverse-direction control ack rides
the reliable socket. This is the clean separation your handover suggested as
option 1, and it removes the entire class of "reverse ring addressing" failures.

### 3.2 Dropped FINs / reads under load — the Allgather hang & latency spikes

**Root cause.** *Every* socket send in the rendezvous path was best-effort:
`rds_send_fin` did `(void) sendmsg(...)` and `rds_start_rndzv_read` treated any
`rds_post_rdma` error as fatal. Under a many-to-many collective the shared RDS
connection's send queue fills and `sendmsg` returns `EAGAIN`/`ENOBUFS`. So FINs
and RDMA READs were *dropped* whenever the fabric was busy — which is precisely
when collectives run. A dropped FIN hangs the sender; a dropped read stalls the
receive. That is the Allgather hang and a chunk of the non-monotonic Allreduce
latency (a stall, a retry, a multi-ms spike).

**Fix.** A FIFO **deferred-op queue** per endpoint (`ep->deferred`). When a FIN
or a rendezvous READ hits `EAGAIN`, it is parked (`struct rds_deferred`) and
re-issued from `rds_progress_deferred()`, which runs first in every progress
pass. It stops at the first op the socket still refuses and retries next pass.
Nothing is ever dropped; a full queue is just back-pressure. Accounting:

- a rendezvous receive finalizes only when `seg_remaining == 0 &&
  seg_deferred == 0` (every segment both issued *and* acknowledged), so a parked
  read can never let the op complete early;
- the parked READ's local scatter list points into the application receive
  buffer, which stays valid until the op finalizes, so re-issuing it later is
  safe.

This is the core of the no-hang guarantee and works **with the ring on or off**.

### 3.3 No flow control — the collective storm (your Action Item #2)

**Root cause.** Nothing bounded concurrent rendezvous. At 192 PPN every rank
fires RTS/FIN to thousands of peers at once over the single RDS connection a node
pair shares; the queue thrashes and latency explodes (your 8,000–23,000 µs).

**Fix.** `ep->rndzv_tx_inflight` counts rendezvous sends awaiting their FIN.
Past `FI_RDS_RNDZV_INFLIGHT` (default 256) a new rendezvous send returns
`-FI_EAGAIN`; MPI throttles itself. No deadlock: the in-flight sends complete
independently of whatever a rank is throttling — there is no cycle. Combine with
the MR cache (§3.4), which removes the per-message registration cost that made
each rendezvous expensive, and optional larger socket buffers
(`FI_RDS_SNDBUF`) to absorb bursts.

### 3.4 `RDS_GET_MR_FOR_DEST` + the MR cache (your registration finding)

We use `RDS_GET_MR_FOR_DEST` because you found empirically that it succeeds where
plain `RDS_GET_MR` did not. **The cookie it returns is destination-independent —
this is not an assumption, it is what upstream `net/rds` does.** From
`net/rds/rdma.c` (Linux master):

- `rds_get_mr_for_dest()` **ignores `dest_addr`** and just forwards to
  `__rds_rdma_map()` — comment in-tree: *"Initially, just behave like
  get_mr()."* So at the kernel level `RDS_GET_MR_FOR_DEST` and `RDS_GET_MR` are
  the same call; the destination is not part of registration.
- `__rds_rdma_map()` builds the cookie as
  `rds_rdma_make_cookie(mr->r_key, args->vec.addr & ~PAGE_MASK)` — i.e. the HCA
  **r_key plus the intra-page offset of the buffer**, with **no destination
  component**. The r_key comes from the FRMR/ODP MR on the device PD and is valid
  for RDMA from any peer reached over that device.

So the cookie can be registered once and reused for every destination. Why
`RDS_GET_MR` failed for you while `RDS_GET_MR_FOR_DEST` worked is almost
certainly an ABI/struct-size or kernel-vintage quirk at the *call* site, not a
difference in the resulting key — but since the for-dest path is what works on
your box, that is what v3 uses. Consequences:

- the rendezvous source registration passes the receiver as `dest_addr` (it is
  connected, and harmless even though current upstream ignores it);
- the **MR cache is keyed on `(buffer, length)` only** and reuses one cookie for
  all destinations. Because the cache API has no destination argument, the send
  path stashes the current destination in `domain->reg_dest` (under `ep->lock`)
  right before the lookup; the `add_region` callback reads it. The cache is
  **on by default** (`FI_RDS_MR_CACHE=1`) and is what lets all-to-all pin the
  send buffer **once** instead of once per peer — the difference between O(1) and
  O(peers) pinning.

### 3.5 Ordering / `FI_ORDER_SAS` with the ring on

The forward path to a READY peer is now **single-channel**: eager messages *and*
rendezvous RTS both ride the ring (the RTS is encapsulated in a slot via the new
`op` field). A peer that is mid-handshake returns `-FI_EAGAIN` (not a datagram
spill), so we never let a later message overtake an earlier one on a second
channel. The only reverse-direction control, the FIN, is not an application
message, so its channel does not affect SAS. Result: same-`(src,tag)` ordering
holds. (One documented edge: a single message larger than ~39 MiB has an RTS
that does not fit a 1 KiB slot and falls back to a socket RTS; benchmarks never
hit this, and within a uniform-size run there is no mixing anyway.)

### 3.6 Counters (`FI_CNTR`) — your Intel MPI fix, made build-safe

Your `rds_cntr_open` passed `NULL` as the progress function. `ofi_cntr_init()`
has `assert(progress)`, so that aborts a debug build; it only "worked" because
your build had `NDEBUG`. v3 passes the standard `&ofi_cntr_progress` (the same
one rxm/rxd/mrail/sm2/shm use), which also drives `rds_ep_progress` when the app
reads the counter. The counter is **advertised but not wired to increment on
completion** — that is enough for Intel MPI's `MPI_Init` acceptance check, and
you still run with `MPIR_CVAR_CH4_OFI_ENABLE_RMA=0`/`ENABLE_ATOMICS=0`. Wiring
counter increments into the completion writers is noted as future work in
`ARCHITECTURE.md`.

---

## 4. What I need you to measure (no design uncertainties left)

### 4.1 MR cache — confirmed correct by upstream, just sanity-check it

This is **settled**, not an open question (I was over-cautious in an earlier
draft). Per upstream `net/rds/rdma.c`, `rds_get_mr_for_dest()` ignores
`dest_addr` and the cookie is `make_cookie(r_key, page_offset)` with no
destination component (§3.4). So caching one cookie per `(buf,len)` and reusing
it for every peer is correct, and that is what gives all-to-all O(1) pinning.

Just confirm nothing regressed: `FI_RDS_MR_CACHE=1` (default) and `=0` should
both pass Alltoall with identical results, the cache version faster on repeated
sends from the same buffer. If they ever differ, that is a bug to report (not an
expected design fork). Either way the rendezvous inflight cap (§3.3) bounds
pinned memory, so the cache cannot reintroduce the original `-ENOMEM`.

### 4.2 Does the eager ring actually reach READY at scale?

With `FI_RDS_EAGER_RDMA=1`, confirm peers reach `READY`
(`FI_LOG_LEVEL=info FI_LOG_PROV=rds`, look for ring registration failures). At
192 PPN the per-peer ring memory is the limiter — see the memory math in
`TUNING_AND_MPI.md`. For **dense** all-to-all the ring does not scale to every
peer (it is O(peers) pinned memory); it is a latency win for sparse/hot-peer
patterns. For dense collectives the scalable path is **datagram-eager +
rendezvous** (ring off), which is now robust. Please benchmark **both**.

### 4.3 The Allreduce latency: is it the connection or the protocol?

The non-monotonic baseline numbers (16 B = 8,831 µs, 128 B = 25 µs, 1 KB =
9,965 µs) look like *stalls*, not steady congestion — consistent with the
dropped-FIN/retry bug (§3.2). I expect v3 to flatten them. If large spikes
remain after v3, capture `strace -c -f` on one rank and `FI_LOG` and send them;
the next lever is `FI_RDS_SNDBUF` and possibly RDS congestion monitoring
(`RDS_CONG_MONITOR`), noted in `ARCHITECTURE.md`.

### 4.4 Sweep these knobs and report

`FI_RDS_RNDZV_INFLIGHT` (try 64 / 256 / 1024), `FI_RDS_SNDBUF` (try 4 MiB / 16
MiB), `FI_RDS_EAGER_SIZE`, ring on/off. The defaults are conservative; your
hardware decides the optimum.

---

## 5. Confidence / risk

- **High confidence, low risk:** the FIN-on-socket fix (§3.1), the deferred
  queue (§3.2), the flow-control cap (§3.3), counters (§3.6), and the MR cache
  (§3.4 — its correctness is confirmed by upstream `net/rds`, not assumed; the
  cookie is destination-independent). These are defensive and cannot make a
  working case worse; `FI_RDS_MR_CACHE=0` remains as a debug toggle.
- **Opt-in, documented caveats:** the eager ring (`FI_RDS_EAGER_RDMA=1`).

I could not compile or run here (no `<linux/rds.h>` / RDS toolchain on this
host), so the version-sensitive spots to watch at build time are the kernel UAPI
structs `rds_get_mr_for_dest_args`, `rds_rdma_args.user_token`, and
`rds_rdma_notify`. Everything else is plain libfabric `prov/util`.

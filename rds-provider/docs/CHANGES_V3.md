# RDS provider v3 — handover (read this first)

> ## ⚡ v3.1 update — the dense-collective Rendezvous deadlock
>
> Your "RDS Rendezvous Deadlock" report (Alltoall/Allgather hang the moment they
> cross into Rendezvous at ≥16 PPN) gets a **two-part provider-level fix**: a real
> head-of-line bug is removed, and the default crossover is moved so the proven
> path carries the common range. Full analysis in **§6**; short version:
>
> **Why buffer/inflight tuning couldn't move it.** `FI_RDS_RNDZV_INFLIGHT` is
> *per rank*, but the RDS connection and its QP are *per node-pair, shared by all
> PPN ranks*. At 16 PPN that funnels ~16×8 = **128 concurrent rendezvous
> RDMA-reads through one QP**, whose hardware outstanding-read limit
> (`max_rd_atomic`, ~16) and send-ring depth are far smaller. The send ring fills
> and `sendmsg` returns `EAGAIN` **regardless of `SO_SNDBUF`** (that bounds bytes,
> not RDMA work-request slots). The provider parks those reads/FINs to retry
> them — and there was the bug.
>
> **Bug (fixed): head-of-line blocking in the deferred-op queue.**
> `rds_progress_deferred` **stopped at the first parked op the socket refused**
> (`return -FI_EAGAIN`). RDS flow-controls *per destination* and parked ops target
> *different* peers, so one congested peer stalled the reads/FINs to every other
> peer. That is the cyclic wait: a node can only drain its incoming RTS storm by
> issuing RDMA reads back to the senders, but those reads sit behind a stuck op,
> so the senders' rendezvous never finalize and their FINs never come — everyone
> spins in `recvmsg`, exactly your `pstack`. **Fix:** walk the *whole* parked
> queue each pass, retry every op independently, keep only the ones that still
> `EAGAIN`. Parked ops are mutually order-independent, so this is safe; a node now
> progresses on every reachable peer regardless of a congested one. (`rds_send_fin`
> also tries the socket immediately instead of queuing behind unrelated parked
> ops; the RDMA-completion batch buffer grew 16→64.)
>
> **Default change: `FI_RDS_EAGER_SIZE` 8 KiB → 64 KiB.** The HOL fix makes
> rendezvous *complete*, but pushing 128 reads through one shared QP is still
> slower than it should be, and your own data shows the **eager path is the better
> choice well past 8 KiB** (3.5× TCP at 32 KiB, clean to 192 PPN). So the crossover
> now sits at 64 KiB: the common collective range rides the path that scales, and
> the failing command (which sets no `FI_RDS_EAGER_SIZE`) now runs its 16 KiB /
> 32 KiB iterations as eager and completes. Rendezvous still applies above 64 KiB
> and is now hang-free; raise `FI_RDS_EAGER_SIZE` toward 1 MiB to confine it
> further. The real fix for >1 MiB without rendezvous (segmented-eager) and the
> QP-pool direction are laid out in §6.

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
   socket rejects with `EAGAIN` is parked and re-issued from the progress engine
   — never dropped. A full send queue becomes *back-pressure*, not a lost
   message. This is the no-hang / no-drop guarantee. (v3.1: the drain retries
   **all** parked ops each pass, not just the head — see the callout / §6.)
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

v3.1 additionally touched: `rds_msg.c` (HOL fix in `rds_progress_deferred`,
inline FIN), `rds_ep.c` (notify buffer 16→64), `rds.h` (`RDS_DEF_EAGER_SIZE`
8 KiB→64 KiB, new `RDS_DEF_INJECT_SIZE`), `rds_attr.c` (advertise the decoupled
`inject_size`). `rds_rma.c` is unchanged.

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

---

## 6. Deep dive: the dense-collective Rendezvous deadlock (your handoff doc)

You isolated it perfectly — Eager fine at 192 PPN, Rendezvous hangs the instant a
dense collective uses it, stuck in `recvmsg`, unmoved by `RNDZV_INFLIGHT=8` and
16 MiB buffers. Here is the full chain and exactly what I changed.

### 6.1 Why it deadlocks (and why it's Rendezvous-only)

Eager is a one-way street: `sendmsg` → the receiver `recvmsg`s and copies out.
Draining an eager message needs **no reverse send**, so a node can always make
progress by reading. That is why it scales to 192 PPN.

Rendezvous is not one-way. To retire one incoming RTS the receiver must **send**
back to the originator — an RDMA READ, then a FIN. "Consume requires send" is the
classic deadlock shape, and three things make it bite here:

1. **Shared QP.** All PPN ranks on a node share **one** RDS connection / QP to
   each peer node. `RNDZV_INFLIGHT` caps rendezvous *per rank*, so the per-QP
   concurrency is `PPN × INFLIGHT` — 128 even at your `INFLIGHT=8`. The QP's
   `max_rd_atomic` (~16 outstanding reads) and send-ring depth are much smaller,
   so the RDS send ring fills and `sendmsg`/the RDMA post returns `EAGAIN`.
   `SO_SNDBUF` is irrelevant — it bounds *bytes in the socket buffer*, not RDMA
   work-request slots on the QP. (This is the part your "not buffer exhaustion"
   observation correctly ruled out — it just wasn't the buffer you can tune.)
2. **The provider parks the EAGAIN'd reads/FINs** (so it never blocks the recv
   drain — good) **but then drained them with head-of-line blocking** (bad): it
   stopped at the first parked op the socket refused. One congested peer froze
   the reads/FINs to all other peers.
3. **Cyclic wait.** Now node A's reads to B are stuck behind a stuck op, so A
   never finalizes B's transfers, so A never FINs B, so B's senders stay
   in-flight and B's queues stay full, so B can't accept A's reads… every node
   spins in `recvmsg`. Deadlock.

### 6.2 The two fixes

- **Remove the head-of-line block** (`rds_progress_deferred`, `rds_msg.c`). Retry
  *every* parked op each pass; keep only the ones that still `EAGAIN`. Parked ops
  are independent across peers and unordered among themselves, so a node now
  drains reads/FINs to every reachable peer even while one peer's QP is saturated.
  The QP saturation becomes *slowness that drains*, not a *cycle that wedges*.
  `rds_send_fin` tries the socket inline (no queuing behind unrelated ops); notify
  batch buffer 16→64.

- **Move the eager/rendezvous crossover to 64 KiB** (`RDS_DEF_EAGER_SIZE`). Your
  data already proved eager is the better path far past 8 KiB; this keeps the
  common collective range on it. Rendezvous (now hang-free) is reserved for the
  large transfers where zero-copy actually pays for its handshake.

### 6.3 Is rendezvous "wrong, or just slow"? — both, now neither fatal

It was *wrong* in one spot (the HOL drain) and *structurally slow* in another
(128 reads through one QP). The HOL bug is a true fix. The QP funnel is not a bug
but a real ceiling: even correct, one QP can't run thousands of concurrent reads
fast. So for the sizes where rendezvous matters, the throughput is capped by that
one QP until the architecture changes — which is your QP-pool idea (§6.5).

### 6.4 The robust >1 MiB answer at the provider level: segmented-eager

Eager is capped at one RDS datagram (~1 MiB). For messages larger than that
*without* rendezvous, the clean provider-level path is **segmented eager**: split
the message into ≤`eager_size` datagrams and reassemble on the receiver. RDS
gives reliable, in-order, per-pair delivery, so reassembly is trivial (no
sequence gaps, no retransmit) — far simpler than rxm's SAR. Sketch:

- new `RDS_OP_EAGER_SEG` carrying `{msg_id, seg_idx, nsegs, total}` in the header;
- the receiver matches the **first** segment to a posted recv, then streams the
  remaining segments (same `msg_id`, arriving in order) into that buffer at the
  running offset; completes on the last;
- no MR registration, no RDMA, no reverse RTS/READ/FIN — only the eager path that
  already scales. Loses zero-copy (two kernel copies) but cannot deadlock.

This is the recommended next implementation step if you need robust >1 MiB before
the QP-pool work. It is **not** in this push (it is a new protocol and I would
rather land it tested than rushed); ~150 lines, isolated to `rds_msg.c`. Say the
word and I will add it.

### 6.5 The QP-pool direction (your architectural note, for later)

Spreading a node-pair's traffic across a small pool of QPs (you suggested 4–8)
directly attacks §6.1.1: it multiplies `max_rd_atomic` and send-ring capacity, so
rendezvous reads stop funnelling through one bottleneck. This needs the RDS
kernel side you said you'll supply later; on the provider side it is mostly
transparent (the provider already addresses peers by sockaddr — the multiplexing
is below us). When you send the RDS patches/version, this is where the provider
hooks in. It does not change any of the v3.1 fixes above; they are prerequisites.

### 6.6 Two scaling notes I noticed while here (not the deadlock)

- `rds_pending_find` / the match queues are **O(n) linear scans**. At a few
  thousand concurrent rendezvous that is O(n²) per collective step — real CPU
  overhead at 1536 ranks, though not a hang. Worth hashing the pending list by
  `id` (a follow-up); flagged so it is on the radar when chasing latency.
- The recv drain budget is 256 messages/pass; fine because MPI re-enters
  progress, but if you see throughput plateaus under the heaviest storms it is a
  cheap knob to raise.

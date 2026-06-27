# RDS libfabric provider — architecture (v3)

A native `FI_EP_RDM` provider over the Linux kernel **RDS** transport (PF_RDS /
SOCK_SEQPACKET) on RoCE v2, for Intel MPI 2021 / MPICH CH4-OFI. The goal is to
beat TCP on collectives (all-to-all first) and scale to 192 PPN across many
nodes by leaning on what RDS already provides — reliable, ordered,
message-boundary datagrams **and** one-sided RDMA with cookies — instead of
reimplementing any of it.

This document is the design reference. For *what changed in v3 and why*, read
[`CHANGES_V3.md`](CHANGES_V3.md) first.

---

## 1. Why RDS, and what it gives us for free

RDS is unusual and a good fit:

- **Reliable, ordered, datagram** semantics per `(src,dst)` socket pair. We do
  not implement acks, retransmit, sequencing, or a CM state machine.
- **One connection (QP) per *node pair*,** multiplexed by all sockets between
  those two nodes. This is the scalability lever: connection state is
  O(nodes²), not O(ranks²). At 192 PPN × N nodes that is the difference between
  feasible and not. The flip side — every rank on a node shares that one QP — is
  also the main contention point, which is why flow control matters (§6).
- **Kernel RDMA with cookies.** `RDS_GET_MR[_FOR_DEST]` pins pages and returns an
  r_key cookie; `RDS_CMSG_RDMA_ARGS` issues a one-sided READ/WRITE; completion
  comes back as an `RDS_CMSG_RDMA_STATUS` notification keyed by a user token.
  This is the zero-copy bulk path, and the cookie *is* the libfabric `mr_key`.

The provider is therefore a thin shim that maps libfabric semantics onto these
primitives and adds exactly two things RDS does not give us: **tag matching**
and **flow control**.

---

## 2. Object model (built on `prov/util`)

```
fi_fabric -> util_fabric
fi_domain -> rds_domain{util_domain} + ofi_mr_cache + custom fi_ops_mr
fi_av     -> ofi_ip_av (IPv4; reverse lookup for FI_SOURCE)
fi_cq     -> util_cq (ofi_cq_progress -> rds_ep_progress)
fi_cntr   -> util_cntr (advertised for Intel MPI; see CHANGES_V3 §3.6)
fi_mr     -> rds_mr (RDS cookie == mr_key); rendezvous src via ofi_mr_cache
fi_ep     -> rds_ep{util_ep}: one PF_RDS/SOCK_SEQPACKET socket + per-peer rings
```

One endpoint owns exactly one RDS socket (forced onto the IB/RoCE transport).
All endpoint state — match queues, pending ops, the deferred queue, the peer/ring
table, the socket — is serialized by `util_ep.lock`. Data progress is **manual**
(busy-poll inside `fi_cq_read`), which is what low-latency MPI wants.

### The single progress point

`rds_ep_progress()` (driven by `ofi_cq_progress` on every `fi_cq_read`) does, in
order:

1. **`rds_progress_deferred()`** — re-issue any parked FIN / rendezvous READ the
   socket previously refused (the back-pressure drain). First, always.
2. **`rds_ring_progress()`** — poll every READY peer ring in memory (no syscall).
3. **drain the socket** with non-blocking `recvmsg` up to a budget:
   - eager datagrams / RTS / FIN → tag-matching engine,
   - `RING_REQ`/`RING_ACK` → ring handshake,
   - `RDS_CMSG_RDMA_STATUS` → RMA and rendezvous-read completions.

---

## 3. Wire framing

Every datagram starts with a fixed `struct rds_hdr` (magic, op, flags, seg_cnt,
tag, data, size, id). Ops: `EAGER`, `RTS`, `FIN`, `RING_REQ`, `RING_ACK`. The
`id` is a per-endpoint monotonic token that ties an RTS to its FIN and an RDMA
op to its completion notification.

The eager-ring slot has its own compact header (`struct rds_ring_slot`:
len, flags, **op**, tag, data, ack, + trailing generation word). The `op` field
(new in the golden patch, kept in v3) lets a slot carry either inline `EAGER`
payload or a whole encapsulated control datagram (an RTS), so the ring can be the
single ordered forward channel (§5).

---

## 4. Transfer tiers

| size | channel | copies | completes when |
|------|---------|--------|----------------|
| 1. tiny/small (≤ ring slot, ring on) | RDMA write into peer ring; receiver polls memory | 1 stage copy, 0 recvmsg | immediately (staged) |
| 2. small/medium (≤ `eager_size`) | RDS datagram `sendmsg` | 1 in + 1 out | immediately (kernel copied) |
| 3. large (> `eager_size`) | RTS → RDMA READ into app buffer → FIN | **zero** on the bulk | on FIN (tx) / on last read (rx) |

Threshold selection in `rds_generic_send()`:

- ring **off** (the scalable default): `≤ eager_size` → tier 2, else tier 3.
- ring **on**: `≤ ring slot` → tier 1; anything larger → tier 3, whose RTS rides
  the ring too, so the forward path stays single-channel (§5). Tier 2 is then
  used only for peers whose ring is disabled or past the resident cap.

### Tier 3 in detail (the zero-copy bandwidth path)

1. Sender registers each ≤1 MiB source chunk (MR cache; §7) and sends an RTS
   carrying `[hdr | seg[]{cookie,off,len}]`.
2. Receiver matches the RTS to a posted receive and issues one **RDMA READ per
   segment straight into the application buffer** — true zero-copy receive.
   Reads that the socket cannot accept right now are **parked and retried**, not
   dropped (§6).
3. When every read is acknowledged (`RDS_CMSG_RDMA_STATUS`), the receiver sends a
   **FIN on the socket**; the sender releases its (cache-refcounted) registration
   and completes its send.

Transfers larger than one kernel segment (1 MiB) are fragmented into ≤1 MiB
segments under one libfabric completion.

---

## 5. The eager ring and ordering (`rds_eager.c`)

Per peer pair: a pre-registered receive ring (`slots × slot_size`) plus an 8-byte
credit cell, cookies exchanged once via a `RING_REQ`/`RING_ACK` handshake over
RDS datagrams. A send stages `[slot-hdr | payload | gen]` and issues one
fire-and-forget RDMA write into the peer's slot; the receiver polls the slot's
trailing **generation word** (written last, so a half-arrived slot is never read)
and feeds the payload into the same tag-matching engine the socket uses.

**Flow control** is credit-based: the consumer index is piggybacked on
reverse-direction traffic (`slot.ack`), so ping-pong / collectives need *zero*
extra RDMA for credit, with an explicit credit write every `slots/2` for
one-directional streams and a flush so a lagging sender always makes progress.

**Resident cap** (`FI_RDS_RING_MAX_PEERS`) bounds pinned ring memory; peers past
the cap use the datagram path. A peer whose ring registration fails is marked
`RDS_PEER_DISABLED` and permanently uses datagrams — never silently dropped.

**Ordering (`FI_ORDER_SAS`).** To a READY peer the ring is the *only* forward
channel: eager payloads and rendezvous RTS both go through it (the RTS is
encapsulated via `slot.op`). A peer mid-handshake returns `-FI_EAGAIN` rather
than spilling onto the datagram channel, so a later message can never overtake an
earlier one across two channels.

**Liveness guard.** Because a CONNECTING peer is held off the datagram channel,
a peer that never answers the `RING_REQ` (its ring registration failed, or it is
past its own resident cap, or a geometry mismatch) would otherwise spin the
sender on `-FI_EAGAIN` forever. So a handshake that does not reach READY within
`RDS_RING_CONNECT_TIMEOUT_MS` (100 ms) marks the peer `RDS_PEER_DISABLED` and
falls back to the datagram path; a geometry mismatch disables it immediately.
The normal handshake is one RTT, so this only fires on genuine failure. The reverse-direction FIN is not an application
message, so it travels the socket without affecting SAS. The one edge case — an
RTS too large for a slot (single message > ~39 MiB at default slot size) falls
back to a socket RTS — is documented and untriggered by the benchmarks.

---

## 6. Reliability & flow control — the no-hang core (`rds_msg.c`)

Two operations the provider must complete itself and cannot return to the
application as `-FI_EAGAIN`: the rendezvous **RDMA READ** (receiver pulling the
payload) and the **FIN** (releasing the sender). Both travel the reliable RDS
socket. When the socket's send queue is momentarily full (`EAGAIN`/`ENOBUFS` —
expected during a collective storm on the shared connection) they are parked on
`ep->deferred` (`struct rds_deferred`, FIFO) and re-issued from
`rds_progress_deferred()` until accepted. A full queue is **back-pressure, not a
lost message** — this is what makes the provider hang-free.

Accounting that makes it correct:

- a rendezvous receive finalizes only when `seg_remaining == 0 &&
  seg_deferred == 0` — every segment both issued and acknowledged;
- a parked READ's local scatter list points into the application receive buffer,
  valid until finalize, so re-issuing later is safe;
- a parked FIN carries the sender's sockaddr + id; nothing to resolve.

**Rendezvous admission control.** `ep->rndzv_tx_inflight` bounds concurrent
rendezvous sends (`FI_RDS_RNDZV_INFLIGHT`, default 256). Past the cap, sends get
`-FI_EAGAIN` and the application self-throttles, so many-to-many collectives
cannot flood the shared RDS connection into the multi-millisecond latency
regime. No deadlock: in-flight sends complete independently of any send a rank is
throttling, so there is no dependency cycle.

**Socket buffers.** `FI_RDS_SNDBUF`/`FI_RDS_RCVBUF` optionally enlarge the RDS
socket buffers (best-effort) to absorb bursts before `EAGAIN` triggers.

---

## 7. Memory registration & the MR cache (`rds_mr.c`)

`fi_mr_reg`/internal registration → `RDS_GET_MR[_FOR_DEST]` → the cookie is
exported verbatim as the libfabric `mr_key`. RMA target addresses are offsets
into the MR, mapped directly onto `rds_rdma_args.remote_vec.addr`.

`RDS_GET_MR_FOR_DEST` is used for the rendezvous source registration because it
is what succeeds on the target box where plain `RDS_GET_MR` did not. The
resulting cookie is **destination-independent** — this is upstream `net/rds`
behaviour, not an assumption: `rds_get_mr_for_dest()` ignores `dest_addr` and
forwards to `__rds_rdma_map()`, which builds the cookie as
`make_cookie(mr->r_key, addr & ~PAGE_MASK)` (device-PD r_key + page offset, no
destination component). So the cookie is valid for RDMA from any peer, and:

- the rendezvous source registration passes the receiver (always connected);
- the **MR cache is keyed on `(buf,len)`** and reuses one cookie across all
  destinations — all-to-all pins the send buffer **once**, not once per peer.
  The send path stashes the destination in `domain->reg_dest` (under `ep->lock`)
  for the argument-less `add_region` callback.

The cache is monitor-backed (invalidates on free/remap), bounded (count ≤ 1024,
size ≤ 256 MiB by default), and **on by default**
(`FI_RDS_MR_CACHE=0` to disable for debugging). With it off, registration is
per-message — always correct, just slower; the rendezvous inflight cap bounds
pinned memory either way, so disabling the cache cannot reintroduce the original
`-ENOMEM`.

The 256-page (1 MiB) per-MR kernel limit is respected by chunking; see
[`RDS_KERNEL_CHANGES.md`](RDS_KERNEL_CHANGES.md) for optional kernel lifts.

---

## 8. Capabilities & ordering

`FI_MSG | FI_TAGGED | FI_RMA | FI_DIRECTED_RECV | FI_SOURCE | FI_MULTI_RECV |
FI_ATOMICS`. `FI_EP_RDM`, `FI_SOCKADDR_IN`, `mr_mode = FI_MR_BASIC`.
Atomics/`FI_MULTI_RECV` are advertised for Intel MPI's getinfo acceptance and the
atomic ops are stubbed (`-FI_ENOSYS`); run with
`MPIR_CVAR_CH4_OFI_ENABLE_ATOMICS=0`, `ENABLE_RMA=0`.

`msg_order` advertises `FI_ORDER_SAS` (+ RAR/RAW/WAR/WAW…). RDS guarantees it on
the socket, and §5 preserves it on the ring. `comp_order` is `NONE`: rendezvous
completes asynchronously when its RDMA notification arrives, so each completion
carries its own `op_context` and MPICH handles the relative ordering.

---

## 9. Concurrency & lock discipline

Everything endpoint-scoped runs under `util_ep.lock`. Completions go through
`ofi_cq_write*` (`cq_lock`). Lock order is acyclic:
`cq.ep_list_lock → ep.lock → cq.cq_lock`, and `ofi_cq_readfrom` runs progress
before taking `cq_lock`, so progress and read never invert. The deferred queue,
the rendezvous counter, the peer table and the MR-cache dest-stash are all
touched only under `ep.lock`, so no new lock is introduced.

---

## 10. Known limitations / future work

- **Eager ring does not scale to dense all-to-all** (O(peers) pinned memory). It
  is a latency win for sparse/hot-peer patterns; dense collectives use the
  datagram-eager + rendezvous path (ring off), which v3 makes robust.
- **Counters advertised but not incremented** on completion (enough for Intel
  MPI init). Wiring `ofi_cntr_inc` into the completion writers is the next step
  for apps that wait on counters.
- **RMA mid-stream `EAGAIN`** (`rds_rma.c`) is not yet routed through the
  deferred queue (RMA is disabled for the MPI benchmarks). The rendezvous read
  path already is.
- **RDS congestion monitoring** (`RDS_CONG_MONITOR`) could feed proactive
  back-off into the rendezvous admission control — a cleaner signal than
  `EAGAIN` at extreme scale.
- **Native atomics** over `RDS_CMSG_ATOMIC_FADD`/`CSWP`.

# RDS libfabric provider — architecture (v2)

A native `FI_EP_RDM` provider over the Linux kernel RDS transport (PF_RDS /
SOCK_SEQPACKET) on RoCE v2, built so MPI (Intel MPI 2021 / MPICH CH4-OFI) gets
RDS-RDMA-V2-class latency and bandwidth without per-message page pinning.

## Design principle

RDS already gives reliable, ordered, message-boundary datagrams **and** one-
sided RDMA with cookies, so the provider is a thin shim, not a reliability
layer. It exposes three transfer tiers, choosing per message:

```
                size                channel                    copies
   ----------------------------     ----------------------     --------------
 1 tiny/small  <= ring slot         RDMA write into peer ring  1 (stage), poll
   (opt-in FI_RDS_EAGER_RDMA)       receiver polls memory      (zero recvmsg)
 2 medium      <= eager_size        RDS datagram sendmsg       1 in + 1 out
 3 large       > eager_size         RTS + RDMA READ (cached)   ZERO copy bulk
```

Tier 1 is the latency path (≈14 µs RTT, beats TCP). Tier 3 is the bandwidth /
big-message path (zero copy, matches RXM). Tier 2 is the always-available
fallback and what runs when the ring is disabled (default).

## Object model (built on `prov/util`)

```
fi_fabric  -> util_fabric
fi_domain  -> rds_domain{util_domain}  + ofi_mr_cache + custom fi_ops_mr
fi_av      -> ofi_ip_av (IPv4, reverse lookup for FI_SOURCE)
fi_cq      -> util_cq (ofi_cq_progress -> rds_ep_progress)
fi_mr      -> rds_mr (RDS cookie == mr_key); rendezvous src via ofi_mr_cache
fi_ep      -> rds_ep{util_ep} (one PF_RDS/SOCK_SEQPACKET socket + peer rings)
```

`rds_ep_progress()` is the single progress point (driven by `fi_cq_read`):
1. `rds_ring_progress()` — poll every READY peer ring in memory (fast path).
2. drain the socket with non-blocking `recvmsg`:
   - eager datagrams / RTS / FIN → tag-matching engine
   - `RING_REQ`/`RING_ACK` → ring handshake
   - `RDS_CMSG_RDMA_STATUS` → RMA / rendezvous-read completions.

## Tier 1: eager RDMA ring (`rds_eager.c`)

Per peer: a pre-registered receive ring (`slots × slot_size`) plus an 8-byte
credit cell, cookies exchanged once via a `RING_REQ`/`RING_ACK` handshake. A
send stages `[hdr | payload | gen]` and issues one fire-and-forget RDMA write
into the peer's ring slot; the receiver polls the slot's trailing generation
word (written last) and feeds the payload into the matching engine. Flow control
is credit-based, piggybacked on reverse traffic (`slot.ack`) plus periodic
explicit credit writes. Resident-peer count is hard-capped
(`FI_RDS_RING_MAX_PEERS`) to bound pinned memory; peers past the cap or
mid-handshake use Tier 2. See `OPTIMIZATION_HANDOVER.md` §3 for the ordering
caveat — it's opt-in (`FI_RDS_EAGER_RDMA=1`).

## Tier 2: datagram eager (`rds_msg.c`)

One `sendmsg` carries `[rds_hdr | payload]`; the receiver `recvmsg`s into a
bounce, parses, matches, copies to the application buffer. Synchronous send
completion (kernel copied the data, buffer reusable). Fully ordered (single
socket). This is the default path and the universal fallback.

## Tier 3: rendezvous with MR cache (`rds_msg.c` + `rds_mr.c`)

Sender registers its source buffer **through the MR cache** (`ofi_mr_cache`,
chunked at the 1 MiB RDS MR limit) and ships a tiny RTS descriptor
`[hdr | seg[]{cookie,len}]`. The receiver matches the RTS to a posted receive
and issues one-sided **RDMA READs straight into the application buffer** (zero
copy), then FINs so the sender can release its (cache-refcounted) registration
and complete. >1 MiB transfers are fragmented into ≤1 MiB segments with a single
completion. The cache makes repeated sends from the same buffer skip
`RDS_GET_MR`, fixing the per-message-pinning `-ENOMEM` and restoring bandwidth.

## Memory registration & RMA semantics

`fi_mr_reg` → `RDS_GET_MR` → the cookie is exported verbatim as the libfabric
`mr_key`. RMA target addresses are offsets into the MR, mapped directly onto
`rds_rdma_args.remote_vec.addr` (matching `rds_rdma_v2.c`, where
`remote_vec.addr == 0` hits the buffer start). RMA completion is asynchronous
via `RDS_RDMA_NOTIFY_ME` + `user_token` → `RDS_CMSG_RDMA_STATUS`, reaped in
progress. (Note: `mr_mode` is advertised as `FI_MR_BASIC` for Intel-MPI
acceptance; MPICH RMA is run disabled, and the internal rendezvous/ring RDMA
computes cookies directly, independent of the advertised mode.)

## Capabilities

`FI_MSG | FI_TAGGED | FI_RMA | FI_DIRECTED_RECV | FI_SOURCE | FI_MULTI_RECV |
FI_ATOMICS` (atomics advertised-but-stubbed for Intel-MPI getinfo; run with
`MPIR_CVAR_CH4_OFI_ENABLE_ATOMICS=0`). `FI_EP_RDM`, `FI_SOCKADDR_IN`.

## Concurrency

All endpoint state (match queues, pending ops, peer table, socket, rings) is
serialized by `util_ep.lock`. Completions go through `ofi_cq_write*`
(`cq_lock`). Lock order is acyclic: `cq.ep_list_lock → ep.lock → cq.cq_lock`,
and `ofi_cq_readfrom` runs progress before taking `cq_lock`, so the progress and
read paths never invert. Data progress is manual (busy-poll in `fi_cq_read`),
which is what low-latency MPI wants.

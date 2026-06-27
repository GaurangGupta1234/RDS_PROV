# libfabric RDS provider (zero-copy RDMA) — v3

A native `FI_EP_RDM` libfabric provider over the Linux kernel **RDS** transport
on RoCE v2, for Intel MPI 2021 collectives. The aim is to beat TCP on
all-to-all and scale to 192 PPN across many nodes, using RDS's reliable ordered
datagrams plus its one-sided RDMA (zero-copy) — not reimplementing reliability.

## Start here

- **[`docs/CHANGES_V3.md`](docs/CHANGES_V3.md)** — the handover: what was broken
  in the golden patch, what v3 fixes and how, and the open questions to measure.
  **Read this first.**
- **[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)** — the full design.
- **[`docs/TUNING_AND_MPI.md`](docs/TUNING_AND_MPI.md)** — build, run, env knobs,
  Intel MPI wiring, benchmark plan.
- **[`docs/RDS_KERNEL_CHANGES.md`](docs/RDS_KERNEL_CHANGES.md)** — kernel
  assumptions and optional `net/rds` patches (proxy QP, MR page cap).

## What v3 fixes (vs the v2 golden patch)

1. **The 16 KiB hang** — the rendezvous FIN was routed over the eager ring where
   its reverse address could not be resolved and was dropped. v3 sends every FIN
   on the reliable RDS socket to the sockaddr the RTS came from. No lookup, no
   drop.
2. **Hangs / latency spikes under collective load** — any FIN or rendezvous RDMA
   READ the full socket rejected with `EAGAIN` was silently lost (the Allgather
   hang, the Allreduce spikes). v3 parks them on a FIFO **deferred queue** and
   re-issues from progress: back-pressure, never a dropped message.
3. **No flow control** — v3 caps concurrent rendezvous sends
   (`FI_RDS_RNDZV_INFLIGHT`) so many-to-many collectives self-throttle instead of
   melting the single shared RDS connection.
4. **MR registration** — keeps `RDS_GET_MR_FOR_DEST` (the registration that works
   on your kernel) and **restores the bounded MR cache**, so all-to-all pins the
   send buffer once instead of once per peer.

The eager ring's forward path is now single-channel and `FI_ORDER_SAS`-correct.

## The transfer tiers

| message size | path | property |
|--------------|------|----------|
| tiny (≤ ring slot, ring on) | RDMA write into peer ring, receiver polls memory | lowest latency |
| small/medium (≤ `eager_size`) | RDS datagram `sendmsg` | scalable default, ordered |
| large (> `eager_size`) | RTS → **RDMA READ** into app buffer → FIN | zero-copy bulk |

## Layout

```
rds-provider/
├── README.md
├── prov/rds/                       drop into <libfabric>/prov/rds
│   ├── configure.m4 · Makefile.include
│   └── src/  rds.h, rds_attr.c, rds_init.c, rds_fabric.c, rds_domain.c,
│             rds_cq.c, rds_mr.c, rds_ep.c, rds_msg.c, rds_rma.c,
│             rds_eager.c, rds_progress.c
├── integration/core-integration.patch   4 one-line libfabric edits
└── docs/  CHANGES_V3.md, ARCHITECTURE.md, TUNING_AND_MPI.md, RDS_KERNEL_CHANGES.md
```

> Authored without a Linux RDS toolchain, so compile in-tree on the target.
> Build-sensitive UAPI: `rds_rdma_args.user_token`, `struct rds_rdma_notify`,
> `struct rds_get_mr_for_dest_args`.

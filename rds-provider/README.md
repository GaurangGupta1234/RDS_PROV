# libfabric RDS provider (zero-copy RDMA) — v2

A native `FI_EP_RDM` libfabric provider over the Linux kernel **RDS** transport
on RoCE v2, for Intel MPI 2021 collectives. v2 fixes the large-message
`-ENOMEM` crash and adds a zero-copy RDMA fast path that targets RDS-RDMA-V2
latency (~14 µs) — at or below TCP, approaching native verbs.

## Start here

- **`docs/OPTIMIZATION_HANDOVER.md`** — what changed in v2, why, how it fixes
  each bottleneck, the tuning knobs, what to benchmark, and next steps. Read
  this first.
- **`docs/ARCHITECTURE.md`** — the three-tier data-path design.
- **`docs/TUNING_AND_MPI.md`** — build, run, env flags, Intel MPI wiring.
- **`docs/RDS_KERNEL_CHANGES.md`** — optional `net/rds` patches (proxy QP, MR
  page cap); the provider runs on a stock kernel.

## The three transfer tiers

| message size | path | property |
|--------------|------|----------|
| small (≤ ring slot, opt-in) | RDMA write into peer ring, **receiver polls memory** | lowest latency, beats TCP |
| medium (≤ `eager_size`) | RDS datagram `sendmsg` | always available, fully ordered |
| large (> `eager_size`) | RTS + **RDMA READ** into app buffer (MR cached) | zero-copy bulk, matches RXM |

## What v2 fixed

1. **`-ENOMEM` crash** — rendezvous no longer pins pages per message; it uses a
   bounded, monitor-backed **MR cache** (always on). Large-message bandwidth
   recovers; `ulimit -l` is no longer exhausted.
2. **Small-message latency** — opt-in **zero-copy eager RDMA ring**
   (`FI_RDS_EAGER_RDMA=1`): memory-polled, no `recvmsg` on the fast path. This
   is the RDS-RDMA-V2 technique generalized to many peers and fed into tag
   matching.

## Layout

```
rds-provider/
├── README.md
├── prov/rds/                       drop into <libfabric>/prov/rds
│   ├── configure.m4 · Makefile.include
│   └── src/  rds.h, rds_attr.c, rds_init.c, rds_fabric.c, rds_domain.c,
│             rds_cq.c, rds_mr.c (MR cache), rds_ep.c, rds_msg.c,
│             rds_rma.c, rds_eager.c (RDMA ring), rds_progress.c
├── integration/core-integration.patch   4 one-line libfabric edits
└── docs/  OPTIMIZATION_HANDOVER.md, ARCHITECTURE.md, TUNING_AND_MPI.md,
          RDS_KERNEL_CHANGES.md
```

## Quick run

```sh
# build (same as v1: drop in prov/rds, apply the patch, configure --enable-rds)
modprobe rds rds_rdma

# A) crash fix + bandwidth (MR cache, default):
mpirun ... -genv FI_PROVIDER rds -genv I_MPI_OFI_PROVIDER rds \
  -genv MPIR_CVAR_CH4_OFI_ENABLE_RMA 0 -genv MPIR_CVAR_CH4_OFI_ENABLE_ATOMICS 0 \
  IMB-MPI1 Allgather

# B) + small-message latency (zero-copy ring):
mpirun ... (as above) -genv FI_RDS_EAGER_RDMA 1  IMB-MPI1 Allreduce
```

> Authored without a Linux RDS toolchain (no `<linux/rds.h>` on the dev host), so
> compile in-tree on the target. Version-sensitive spots: the kernel UAPI fields
> `rds_rdma_args.user_token` and `rds_rdma_notify`.

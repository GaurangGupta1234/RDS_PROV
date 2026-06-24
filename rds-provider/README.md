# libfabric RDS provider (zero-copy RDMA) — deliverable

A native libfabric provider that runs `FI_EP_RDM` (message, tagged, and RMA)
over the Linux kernel **RDS** transport on RoCE v2, using the **RDS RDMA cookie**
mechanism for true **zero-copy** bulk transfer — the same path that gave
`rds_rdma_v2.c` ~14 µs small-message and ~212 µs 1 MB RTT. Goal: run Intel MPI
2021 collectives (alltoall, allgather, …) at RDS-RDMA performance.

## What's here

```
rds-provider/
├── README.md                     ← this file
├── prov/rds/                     ← the provider (drop into <libfabric>/prov/rds)
│   ├── configure.m4
│   ├── Makefile.include
│   └── src/
│       ├── rds.h                 backbone: structs, wire framing, helpers, RDS ABI
│       ├── rds_attr.c            capability / attribute tables (fi_info)
│       ├── rds_init.c            provider registration, fi_getinfo, params
│       ├── rds_fabric.c          fabric open/close
│       ├── rds_domain.c          domain open/close, installs MR ops
│       ├── rds_cq.c              completion queue (util_cq)
│       ├── rds_mr.c              memory registration → cookie (zero-copy enabler)
│       ├── rds_ep.c              endpoint, RDS socket, setname/getname, bind
│       ├── rds_msg.c             tagged/msg path, matching, eager + rendezvous
│       ├── rds_rma.c             fi_read / fi_write (one-sided zero copy)
│       └── rds_progress.c        socket drain + RDMA completion reaping
├── integration/
│   └── core-integration.patch    ← 4 one-line edits to libfabric build/glue
└── docs/
    ├── RDS_PROVIDER_DESIGN.md    full architecture & data-path design
    ├── RDS_KERNEL_CHANGES.md     net/rds patches (proxy QP, MR cap, …) + what's optional
    ├── LIBFABRIC_INTEGRATION.md  step-by-step in-tree integration & build
    └── INTEL_MPI_USAGE.md        Intel MPI 2021 wiring, tuning, collectives
```

## Quick start (on the target Linux RoCE box)

```sh
# 1. drop in + wire up
cp -r rds-provider/prov/rds  <libfabric>/prov/rds
cd <libfabric> && git apply <.../integration/core-integration.patch>

# 2. build
./autogen.sh && ./configure --prefix=/opt/libfabric-rds --enable-rds
make -j && make install

# 3. load kernel modules + verify
sudo modprobe rds rds_rdma
FI_PROVIDER=rds /opt/libfabric-rds/bin/fi_info -p rds

# 4. run Intel MPI over it
export I_MPI_OFI_LIBRARY_INTERNAL=0 FI_PROVIDER=rds I_MPI_FABRICS=ofi
export LD_LIBRARY_PATH=/opt/libfabric-rds/lib:$LD_LIBRARY_PATH
mpirun -n 2 -ppn 1 -hosts vm2,vm3 ./IMB-MPI1 alltoall
```

## The one-paragraph design

RDS already gives reliable, ordered, message-boundary datagrams **and** 1-sided
RDMA with cookies, so the provider is a thin shim, not a reliability layer.
Small messages go **eager** (one `sendmsg` of `[header|payload]`, one kernel
copy — cheapest for short MPI traffic). Large messages go **rendezvous**: the
sender registers its source buffer (`RDS_GET_MR` → cookie), ships a tiny RTS,
and the receiver issues one-sided **RDMA READs straight into the application
buffer** — zero copy. `fi_read`/`fi_write` map directly to `RDS_CMSG_RDMA_ARGS`.
Completions come back as `RDS_CMSG_RDMA_STATUS` (via `user_token` +
`RDS_RDMA_NOTIFY_ME`), reaped in the progress loop. The MR cookie is exported
verbatim as the libfabric key; MR uses offset (scalable) semantics that match
`rds_rdma_args.remote_vec.addr` exactly. The 1 MiB RDS limits are worked around
by chunking large transfers into ≤1 MiB RDMA segments with a single completion.

See **docs/RDS_PROVIDER_DESIGN.md** for the full writeup.

## Important caveats for the integrating agent

- **Authored on Windows without `<linux/rds.h>`, so it was not compiled here.**
  It targets the same `prov/util` surface as `prov/udp` and was written against
  current libfabric master APIs. Compile in-tree on the target; the only
  version-sensitive spots are the kernel UAPI struct fields
  (`rds_rdma_args.user_token`, `rds_rdma_notify`).
- **`RDS_GET_MR` needs a socket bound to the RoCE device and (on a stock kernel)
  a live connection to post the FRMR.** MPI's init barrier pre-connects peers,
  which covers it; the robust fix is the **proxy QP** patch in
  `docs/RDS_KERNEL_CHANGES.md` §3 (the item you flagged from the frmr file). No
  provider change is needed to benefit from it.
- **App RMA windows > 1 MiB** need the MR page-cap kernel patch
  (`RDS_KERNEL_CHANGES.md` §2); point-to-point messages of any size already work
  via segmented rendezvous.

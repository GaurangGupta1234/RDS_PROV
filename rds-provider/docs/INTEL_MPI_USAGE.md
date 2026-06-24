# Running Intel MPI 2021 over the RDS provider

Intel MPI 2021 uses MPICH's CH4 / OFI (libfabric) netmod. Point it at our
provider and it will drive `fi_tsend`/`fi_trecv` for point-to-point and
collectives and `fi_read`/`fi_write` for one-sided — all of which map onto the
zero-copy RDS RDMA path for large transfers.

---

## 1. Point Intel MPI at this libfabric

Intel MPI ships its own libfabric. Override it with the one you built with the
RDS provider:

```sh
export I_MPI_OFI_LIBRARY_INTERNAL=0           # don't use the bundled libfabric
export LD_LIBRARY_PATH=/opt/libfabric-rds/lib:$LD_LIBRARY_PATH
export FI_PROVIDER_PATH=/opt/libfabric-rds/lib/libfabric   # if built as DL
export I_MPI_FABRICS=ofi                       # use OFI (not the shm:ofi default for inter-node)
export FI_PROVIDER=rds                          # select our provider
```

Confirm selection:
```sh
I_MPI_DEBUG=4 FI_LOG_LEVEL=warn FI_LOG_PROV=rds mpirun -n 2 -ppn 1 \
    -hosts vm2,vm3 ./IMB-MPI1 pingpong
```
`I_MPI_DEBUG=4` prints the chosen provider; you want to see `rds`.

## 2. Addressing

The RDS provider uses `FI_SOCKADDR_IN` (the RoCE interface IP). Make sure each
rank binds to its RoCE IP, e.g. via the host list mapping to `10.4.0.x`. If
needed, constrain the NIC:

```sh
export FI_RDS_IFACE=...           # (only if you add iface selection; v1 binds
                                  #  to the src_addr libfabric resolves)
export I_MPI_OFI_PROVIDER=rds
```

Intel MPI exchanges endpoint addresses over PMI during `MPI_Init`, then inserts
them into the AV. The init barrier also pre-establishes RDS connections, which
is what lets memory registration / rendezvous work without a proxy QP on a stock
kernel (see `RDS_KERNEL_CHANGES.md` §3).

## 3. Tuning for the zero-copy path

| variable | effect | suggested |
|----------|--------|-----------|
| `FI_RDS_EAGER_SIZE` | crossover from copy-eager to zero-copy RDMA rendezvous | start 8192; sweep 4096–65536 |
| `I_MPI_OFI_RDMA` / rendezvous threshold | MPICH's own eager→rndzv switch | align near `FI_RDS_EAGER_SIZE` |
| `FI_UNIVERSE_SIZE` | sizes AV/connection tables | set to total ranks |
| `I_MPI_PIN`, core binding | keep busy-poll progress on dedicated cores | pin 1 rank/core |

Because progress is manual (busy-poll in `fi_cq_read`), keep MPI in polling
mode (`I_MPI_WAIT_MODE=0`, the default) for lowest latency — this mirrors the
busy-poll your `rds_rdma_v2.c` used to hit 14 µs.

## 4. Collectives (alltoall, allgather, ...)

These are exactly the workloads this provider is tuned for:

- Small-message collectives → eager (one copy each way), low latency.
- Large-message collectives → each pairwise transfer becomes an RTS + RDMA READ
  straight into the destination buffer (zero copy), and >1 MiB messages are
  transparently fragmented into ≤1 MiB RDMA-read segments.
- One window registered per rank serves reads/writes from *all* peers (a single
  cookie is HCA-valid for every initiator), so all-to-all does not multiply
  registrations.

Recommended benchmark sweep (matches your latency report methodology):

```sh
# IMB collectives
mpirun -n N -ppn 1 -hosts <list> ./IMB-MPI1 alltoall allgather -npmin N
# OSU micro-benchmarks
mpirun -n N ./osu_alltoall ; ./osu_allgather ; ./osu_bw ; ./osu_latency
```

Compare `osu_latency`/`osu_bw` against the `rds_rdma_v2` and Native IB rows in
your report — the RMA/large path should track RDS RDMA V2; small messages will
sit between RDS-copy and RDS-RDMA depending on `FI_RDS_EAGER_SIZE`.

## 5. Troubleshooting

| symptom | cause / fix |
|---------|-------------|
| `fi_getinfo` returns no `rds` | `modprobe rds rds_rdma`; check `FI_PROVIDER_PATH` |
| provider selected but hangs at init | RoCE IP mismatch; verify the bound IP maps to the IB device (same as `rds_rdma_v2.c`) |
| `RDS_GET_MR` / registration errors in `FI_LOG` | register-before-connect; ensure MPI init barrier ran, or apply the proxy-QP patch |
| `EMSGSIZE` on huge one-sided windows | window > 1 MiB needs the MR page-cap kernel patch (`RDS_KERNEL_CHANGES.md` §2) |
| poor small-msg latency | lower `FI_RDS_EAGER_SIZE` won't help <1 KB (copy dominates); ensure busy-poll/pinning |
| Intel MPI picks a different provider | `FI_PROVIDER=rds` + `I_MPI_OFI_PROVIDER=rds`, and confirm with `I_MPI_DEBUG=4` |

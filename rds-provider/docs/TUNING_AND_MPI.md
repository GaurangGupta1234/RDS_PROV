# Build, run, tune — RDS provider with Intel MPI 2021

## 1. Build (in-tree)

```sh
cp -r rds-provider/prov/rds  <libfabric>/prov/rds
cd <libfabric> && git apply <.../integration/core-integration.patch>
./autogen.sh
./configure --prefix=/opt/libfabric-rds --enable-rds
make -j && make install
```

`integration/core-integration.patch` makes the four one-line edits to
`configure.ac`, `Makefile.am`, `include/ofi_prov.h`, `src/fabric.c`. The new
`rds_eager.c` is already in `prov/rds/Makefile.include`. (No `fi_rds.7` man page
is shipped; the Makefile fragment does not reference one.)

Requires `<linux/rds.h>` with `rds_rdma_args.user_token`; `configure` warns if
absent (see `RDS_KERNEL_CHANGES.md` §0).

## 2. Load modules and sanity-check

```sh
modprobe rds rds_rdma
FI_PROVIDER=rds /opt/libfabric-rds/bin/fi_info -p rds      # expect FI_EP_RDM "rds"
# data-path smoke test (no MPI):
FI_PROVIDER=rds fi_rdm_tagged_pingpong -p rds 10.4.0.20            # server
FI_PROVIDER=rds fi_rdm_tagged_pingpong -p rds 10.4.0.17 10.4.0.20 # client
FI_RDS_EAGER_RDMA=1 FI_PROVIDER=rds fi_rdm_tagged_pingpong ...     # exercise the ring
```

## 3. Intel MPI wiring

```sh
export I_MPI_OFI_LIBRARY_INTERNAL=0
export LD_LIBRARY_PATH=/opt/libfabric-rds/lib:$LD_LIBRARY_PATH
export FI_PROVIDER_PATH=/opt/libfabric-rds/lib/libfabric    # if built as DL
export I_MPI_FABRICS=ofi
export FI_PROVIDER=rds  I_MPI_OFI_PROVIDER=rds
# the provider does not implement atomics/RMA-windows yet:
export MPIR_CVAR_CH4_OFI_ENABLE_ATOMICS=0
export MPIR_CVAR_CH4_OFI_ENABLE_RMA=0
```

## 4. Runtime knobs

| env var | meaning | default |
|---------|---------|---------|
| `FI_RDS_EAGER_RDMA` | enable the zero-copy eager ring (latency) | 0 |
| `FI_RDS_RING_SLOTS` | slots/peer ring (power of two) | 16 |
| `FI_RDS_RING_SLOT_SIZE` | bytes/slot (caps ring-eager payload) | 1024 |
| `FI_RDS_RING_MAX_PEERS` | resident rings (bounds pinned memory) | 256 |
| `FI_RDS_EAGER_SIZE` | datagram-eager vs rendezvous threshold | 8192 |
| `FI_MR_CACHE_MAX_COUNT` | rendezvous MR cache entry cap | 1024 |
| `FI_MR_CACHE_MAX_SIZE` | rendezvous MR cache byte cap | 256 MiB |
| `FI_LOG_LEVEL=info FI_LOG_PROV=rds` | provider diagnostics | off |

### Memory math (before raising ring limits at scale)

Pinned ≈ `FI_RDS_RING_MAX_PEERS × FI_RDS_RING_SLOTS × FI_RDS_RING_SLOT_SIZE`
(+ MR cache ≤ `FI_MR_CACHE_MAX_SIZE`). Defaults: 256 × 16 × 1024 ≈ 4 MiB/rank of
ring + ≤256 MiB cache. At 192 PPN ensure `ulimit -l` covers
`PPN × (ring + cache working set)`. Raise `ring_max_peers` only if memlock
allows; peers beyond the cap silently use the (correct, slower) datagram path.

## 5. Benchmark plan

```sh
# crash-fix / bandwidth (ring off):
mpirun ... IMB-MPI1 Allgather Alltoall -msglog 0:22
# latency (ring on):
mpirun ... -genv FI_RDS_EAGER_RDMA 1  IMB-MPI1 Allreduce -msglog 0:15
mpirun ... -genv FI_RDS_EAGER_RDMA 1  ./osu_latency ./osu_bw
```

Compare against your tcp/rxm columns. Targets: large-message Allgather no longer
crashes and approaches RXM bandwidth; small-message latency with the ring on
drops from ~35 µs toward ~14–18 µs (≤ TCP).

## 6. If the ring doesn't help

Check `FI_LOG_LEVEL=info FI_LOG_PROV=rds`:
- peers never reach `READY` → handshake failing (geometry mismatch across ranks
  — keep `FI_RDS_RING_*` identical everywhere — or `RDS_GET_MR` failing on ring
  buffers: see proxy-QP note in `RDS_KERNEL_CHANGES.md` §3).
- everything falling back to datagram → likely `FI_RDS_RING_MAX_PEERS` too small
  for the communication pattern, or messages larger than the slot payload
  (raise `FI_RDS_RING_SLOT_SIZE`).

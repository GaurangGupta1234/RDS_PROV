# Build, run, tune — RDS provider (v3) with Intel MPI 2021

## 1. Build (in-tree)

```sh
cp -r rds-provider/prov/rds  <libfabric>/prov/rds      # or build the libfabric/ tree directly
cd <libfabric> && git apply <.../integration/core-integration.patch>
./autogen.sh
./configure --prefix=/opt/libfabric-rds --enable-rds
make -j && make install
```

`integration/core-integration.patch` makes the four one-line edits to
`configure.ac`, `Makefile.am`, `include/ofi_prov.h`, `src/fabric.c`. All provider
sources (including `rds_eager.c`) are listed in `prov/rds/Makefile.include`.

Requires `<linux/rds.h>` with `rds_rdma_args.user_token`, `struct rds_rdma_notify`
and `struct rds_get_mr_for_dest_args`; `configure` warns if `user_token` is
absent. (The two repo copies — `libfabric/prov/rds` and `rds-provider/prov/rds` —
are kept identical; build either.)

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
# the provider does not implement atomics / RMA windows:
export MPIR_CVAR_CH4_OFI_ENABLE_ATOMICS=0
export MPIR_CVAR_CH4_OFI_ENABLE_RMA=0
```

## 4. Runtime knobs

| env var | meaning | default |
|---------|---------|---------|
| `FI_RDS_EAGER_SIZE` | datagram-eager vs rendezvous threshold (bytes) | 65536 |
| `FI_RDS_MR_CACHE` | cache rendezvous source registrations (0/1) | 1 |
| `FI_RDS_RNDZV_INFLIGHT` | max concurrent rendezvous sends before `-FI_EAGAIN` (flow control) | 256 |
| `FI_RDS_SNDBUF` | `SO_SNDBUF` bytes on the RDS socket (0 = kernel default) | 0 |
| `FI_RDS_RCVBUF` | `SO_RCVBUF` bytes on the RDS socket (0 = kernel default) | 0 |
| `FI_RDS_EAGER_RDMA` | enable the zero-copy eager ring (latency, opt-in) | 0 |
| `FI_RDS_RING_SLOTS` | slots/peer ring (power of two) | 16 |
| `FI_RDS_RING_SLOT_SIZE` | bytes/slot (caps ring-eager payload) | 1024 |
| `FI_RDS_RING_MAX_PEERS` | resident rings (bounds pinned memory) | 256 |
| `FI_MR_CACHE_MAX_COUNT` / `FI_MR_CACHE_MAX_SIZE` | MR cache bounds | 1024 / 256 MiB |
| `FI_LOG_LEVEL=info FI_LOG_PROV=rds` | provider diagnostics | off |

### Memory math (before raising limits at scale)

- **MR cache**: bounded by `FI_MR_CACHE_MAX_SIZE` (256 MiB default) per rank.
- **Eager ring** (only if `FI_RDS_EAGER_RDMA=1`): pinned ≈
  `FI_RDS_RING_MAX_PEERS × FI_RDS_RING_SLOTS × FI_RDS_RING_SLOT_SIZE` per rank
  (defaults ≈ 4 MiB) plus equal unregistered staging. At 192 PPN that is the
  binding constraint — for **dense** all-to-all the ring does not fit every
  peer; run ring **off** there (datagram-eager + rendezvous is the scalable
  path and is now robust).

Ensure `ulimit -l` covers `PPN × (MR-cache working set + ring set)`.

## 5. Benchmark plan

```sh
# Scalable default (ring off): robustness + bandwidth
mpirun ... IMB-MPI1 Alltoall Allgather Allreduce -msglog 0:22

# Latency (ring on), good for sparse/hot-peer patterns and small msgs:
mpirun ... -genv FI_RDS_EAGER_RDMA 1  IMB-MPI1 Allreduce -msglog 0:15
mpirun ... -genv FI_RDS_EAGER_RDMA 1  ./osu_latency ./osu_bw

# Cache sanity check (both must agree; cache version faster on repeated sends):
mpirun ... -genv FI_RDS_MR_CACHE 1  IMB-MPI1 Alltoall    # default
mpirun ... -genv FI_RDS_MR_CACHE 0  IMB-MPI1 Alltoall    # debug toggle
```

Targets: Alltoall/Allgather no longer hang at any size; Allreduce latency is flat
(no multi-ms spikes) and beats TCP; large-message bandwidth approaches RXM.

## 6. If something still misbehaves

`FI_LOG_LEVEL=info FI_LOG_PROV=rds`:

- **Hang at the rendezvous threshold** → should be gone (FIN reliability + read
  retry). If not, capture `strace -c -f` on the stuck rank and the FI log.
- **Allgather/Alltoall corruption with the cache on but fine with `FI_RDS_MR_CACHE=0`**
  → unexpected: upstream RDS cookies are destination-independent
  (`make_cookie(r_key, page_offset)`), so on/off must agree. This would be a bug
  to report, not a kernel quirk; run with the cache off meanwhile.
- **Ring never helps** → peers not reaching `READY` (geometry mismatch — keep
  `FI_RDS_RING_*` identical on all ranks — or ring registration failing), or the
  pattern is too dense for `FI_RDS_RING_MAX_PEERS`.
- **Latency spikes under heavy collectives** → raise `FI_RDS_SNDBUF` (4–16 MiB)
  and/or lower `FI_RDS_RNDZV_INFLIGHT` to throttle the storm sooner.
- **Dense Alltoall/Allgather hangs when it crosses into Rendezvous** (the
  deadlock fixed in v3.1, CHANGES_V3.md §6) → the head-of-line fix should clear
  it. If a residual stall remains on a given fabric, raise `FI_RDS_EAGER_SIZE`
  toward 1 MiB so the collective range stays on the eager path (which scales to
  192 PPN); `SO_SNDBUF` will *not* help here — the limit is RDMA work-request
  slots on the shared per-node-pair QP, not socket bytes.

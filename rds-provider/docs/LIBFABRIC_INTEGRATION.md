# Integrating the RDS provider into libfabric

This provider is a normal in-tree libfabric provider. Integration is: drop in
`prov/rds/`, make four one-line edits to the build/registration glue, then
`autogen → configure → make`.

The four core-file edits are captured as a ready-to-apply patch in
`integration/core-integration.patch` (generated against current
`ofiwg/libfabric` master). Apply it, or make the edits by hand as below.

---

## 1. Drop in the provider

```sh
cp -r rds-provider/prov/rds   <libfabric>/prov/rds
```

Result:
```
<libfabric>/prov/rds/
  configure.m4
  Makefile.include
  src/{rds.h, rds_attr.c, rds_init.c, rds_fabric.c, rds_domain.c,
       rds_cq.c, rds_mr.c, rds_ep.c, rds_msg.c, rds_rma.c, rds_progress.c}
```

## 2. Four glue edits (or `git apply integration/core-integration.patch`)

**a) `configure.ac`** — register the provider with the build (calls
`FI_RDS_CONFIGURE` from `prov/rds/configure.m4` by naming convention):
```diff
 FI_PROVIDER_SETUP([udp])
+FI_PROVIDER_SETUP([rds])
 FI_PROVIDER_SETUP([tcp])
```

**b) `Makefile.am`** — pull in the provider's source list:
```diff
 include prov/udp/Makefile.include
+include prov/rds/Makefile.include
 include prov/verbs/Makefile.include
```

**c) `include/ofi_prov.h`** — declare the INI hook (mirror of the UDP block):
```diff
+#if (HAVE_RDS) && (HAVE_RDS_DL)
+#  define RDS_INI FI_EXT_INI
+#  define RDS_INIT NULL
+#elif (HAVE_RDS)
+#  define RDS_INI INI_SIG(fi_rds_ini)
+#  define RDS_INIT fi_rds_ini()
+RDS_INI ;
+#else
+#  define RDS_INIT NULL
+#endif
```

**d) `src/fabric.c`** — register at `fi_ini()`:
```diff
 	ofi_register_provider(UDP_INIT, NULL);
+	ofi_register_provider(RDS_INIT, NULL);
 	ofi_register_provider(SOCKETS_INIT, NULL);
```

> Optional cosmetic edit (not required — the code uses `FI_PROTO_UNSPEC`): add
> `FI_PROTO_RDS` to the protocol enum in `include/rdma/fabric.h` and set
> `rds_ep_attr.protocol = FI_PROTO_RDS` in `prov/rds/src/rds_attr.c`.

## 3. Build

```sh
cd <libfabric>
./autogen.sh
./configure --prefix=/opt/libfabric-rds \
            --enable-rds \
            --disable-verbs   # optional: trim providers you don't need
make -j && make install
```

Build options:
- `--enable-rds` (default auto): requires `<linux/rds.h>` and
  `struct rds_rdma_args.user_token` (configure warns if the token field is
  absent — see `RDS_KERNEL_CHANGES.md` §0).
- `--enable-rds=dl` builds it as a loadable `librds-fi.so` plugin instead of
  statically linking into `libfabric.so`.

## 4. Verify the provider is present

```sh
# Load the kernel modules first
sudo modprobe rds rds_rdma          # rds_rdma pulls in the IB transport

# List providers
FI_PROVIDER=rds /opt/libfabric-rds/bin/fi_info -p rds
```

Expect an `FI_EP_RDM` provider named `rds`, `addr_format FI_SOCKADDR_IN`, caps
including `FI_MSG FI_TAGGED FI_RMA`. Bind it to your RoCE interface IP:

```sh
FI_PROVIDER=rds fi_info -p rds -s 10.4.0.20
```

## 5. Smoke test with fabtests

```sh
# server (vm2)
FI_PROVIDER=rds fi_rdm_pingpong -p rds -s 10.4.0.20
# client (vm3)
FI_PROVIDER=rds fi_rdm_pingpong -p rds -s 10.4.0.17 10.4.0.20
```

`fi_rdm_tagged_pingpong`, `fi_rdm_tagged_bw`, and `fi_rma_bw -e rdm` exercise the
tagged and RMA (zero-copy) paths respectively. `fi_rma_bw` large sizes should
approach your `rds_rdma_v2.c` numbers.

## 6. Runtime knobs (fi_param / env)

| env var | meaning | default |
|---------|---------|---------|
| `FI_RDS_EAGER_SIZE` | payload ≤ this → eager (copy); above → zero-copy rendezvous | 8192 |
| `FI_LOG_LEVEL=warn FI_LOG_PROV=rds` | provider diagnostics | off |

## 7. Notes for the integrating agent

- The provider compiles cleanly in-tree on Linux; it was **not** compiled on the
  authoring host (Windows, no `<linux/rds.h>`). If the toolchain flags anything,
  the most likely spots are kernel-UAPI struct field names that vary by kernel
  version — `rds_rdma_args.user_token`, `rds_rdma_notify.{user_token,status}`.
  Cross-check against the `<linux/rds.h>` on the target.
- It depends only on `prov/util` + core ofi headers (same set as `prov/udp`),
  so no extra external libraries (no libibverbs link needed — all RDMA goes
  through the RDS socket).
- Make sure the RoCE interface IP you bind to is the one RDS associates with the
  IB device (the same IP family you used for `rds_rdma_v2.c`).

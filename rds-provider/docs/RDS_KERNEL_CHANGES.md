# RDS kernel (`net/rds`) changes for the libfabric RDS provider

This document lists every assumption the provider makes about the kernel RDS
module, which of them stock upstream already satisfies, and the **optional**
patches that lift performance/size limits. The provider is written to run on a
**stock** RDS module first (degrading gracefully), so you can bring it up before
touching the kernel.

> Reference tree: `linux/net/rds/` and UAPI `include/uapi/linux/rds.h`.

---

## 0. What the provider requires from a *stock* kernel (no patch)

1. `PF_RDS`/`SOCK_SEQPACKET` sockets, `SO_RDS_TRANSPORT = RDS_TRANS_IB`.
2. `RDS_GET_MR` (opt 2) returning an `rds_rdma_cookie_t`, and `RDS_FREE_MR`
   (opt 3).  — `net/rds/rdma.c: rds_get_mr() / rds_free_mr()`.
3. `RDS_CMSG_RDMA_ARGS` one-sided ops with `rds_rdma_args`.
   — `net/rds/rdma.c: rds_cmsg_rdma_args()`, `rds_rdma_prepare()`.
4. **`rds_rdma_args.user_token`** + **`RDS_RDMA_NOTIFY_ME`** producing an inline
   `RDS_CMSG_RDMA_STATUS` (`struct rds_rdma_notify{user_token,status}`) on
   recvmsg.  — `net/rds/recv.c: rds_recvmsg()` drains `rs_notify_queue`;
   `net/rds/send.c`/`rdma.c` queue the notifier on completion.

   The provider's `configure.m4` checks `struct rds_rdma_args.user_token` and
   warns if absent. Your custom module already has it (per the benchmark UAPI).

If (1)–(4) hold, eager messaging, rendezvous, and RMA all work — subject to the
1 MiB ceiling below.

> **v3 registration note.** The provider registers rendezvous source buffers
> with **`RDS_GET_MR_FOR_DEST` (opt 7)** rather than plain `RDS_GET_MR`, because
> that is what succeeds on the target box. In upstream `net/rds/rdma.c` the two
> are the *same* call — `rds_get_mr_for_dest()` ignores `dest_addr` and forwards
> to `__rds_rdma_map()` (comment: *"Initially, just behave like get_mr()."*) — so
> the "`rds_get_mr` doesn't work but `rds_get_mr_for_dest` does" symptom is an
> ABI/struct-size or kernel-vintage quirk at the call site, not a difference in
> the resulting key. The cookie is `make_cookie(r_key, page_offset)` with **no
> destination component**, so it is destination-independent and the provider
> caches it across peers (correct by construction, not an assumption).

---

## 1. The 1 MiB message ceiling (copy path)  — `RDS_MAX_MSG_SIZE`

**Symptom:** `EMSGSIZE` for `sendmsg` payloads ≥ 1 MiB (matches your report:
RDS TCP/IB copy fail at 2 MiB).

**Where:** `net/rds/send.c: rds_sendmsg()` checks the total against the
fragment/size limit; the effective cap is 1 MiB.

**Provider handling (no patch needed):** the provider never sends an eager
datagram larger than `eager_size` (≤ 8 KiB by default), and large messages use
the rendezvous RDMA path. So you do **not** need to raise this for MPI
point-to-point.

**Optional patch** (only if you want a larger *eager*/copy datagram): raise the
size limit constant and rebuild. Keep `rds_eager_size` ≤ the new value.

---

## 2. The 256-page (1 MiB) MR ceiling  — `RDS_GET_MR`

**Symptom:** `RDS_GET_MR` fails for regions > 256 pages (your report: MR
registration fails at 1 MiB; "limits the number of pinned pages to 256").

**Where:** `net/rds/rdma.c: __rds_rdma_map()` caps `nr_pages`; the IB FRMR pool
(`net/rds/ib_frmr.c`) also has a max page count per MR
(`pool->max_pages` / `RDS_MR_1M_POOL` sizing in `net/rds/ib_mr.h`).

**Provider handling (no patch needed for messaging):** the rendezvous send path
registers the source buffer in ≤1 MiB chunks (`RDS_KERNEL_SEG_MAX`), one cookie
per chunk, and the receiver issues one RDMA read per chunk. So multi-MiB MPI
messages work on a stock kernel.

**Patch needed for app RMA windows > 1 MiB** (`MPI_Win_create` of a large
window): a single `fi_mr_key` carries one cookie, so the *whole* window must be
one MR. To support that:

```
net/rds/ib_mr.h     : bump RDS_MR_1M_POOL page budget / pool->max_pages
net/rds/rdma.c      : raise the nr_pages cap in __rds_rdma_map()
net/rds/ib_frmr.c   : ensure FRMR page_list / max_pages matches
```

Validate against your HCA's `max_fast_reg_page_list_len`
(`ibv_query_device`). On Columbiaville/irdma this is typically large enough for
several MiB.

---

## 3. Proxy QP for MR registration (FRMR posting)  — the item you flagged

**Problem:** `RDS_GET_MR` builds an FRMR by posting an `IB_WR_REG_MR` work
request. In stock RDS the FRMR is posted on a *data connection's* QP
(`net/rds/ib_frmr.c: rds_ib_post_reg_frmr()` uses `ibmr->ic->i_cm_id->qp` /
the connection's QP). If the application registers a buffer **before any RDS
connection to a peer exists** (very common: a receiver registers its window at
init, before traffic), there may be no QP to post the FASTREG WR on →
registration can stall or fail (`ENOTCONN`/`EAGAIN`).

This is the "**proxy QP**" idea: give the IB device a dedicated, always-up QP
used solely to post FRMR registration/invalidation WRs, independent of any data
connection.

### Provider-side mitigation (works on stock kernel, no patch)
MPI performs an init barrier/allgather that exchanges small eager messages,
which establishes the RDS connections to all peers **before** windows are
registered or large rendezvous transfers happen. The provider relies on this
ordering. If you ever register before first contact, send one 0-byte warmup
eager to each peer first.

### Robust kernel patch (recommended for production)
Add a per-`rds_ib_device` proxy QP + CQ created at device attach, and post FRMR
reg/inval WRs on it instead of the connection QP:

```
net/rds/ib.h        : struct rds_ib_device { ... struct ib_qp *fastreg_qp;
                                                  struct ib_cq *fastreg_cq; ... }
net/rds/ib.c        : in rds_ib_add_one(): create a small RC/UD QP bound to the
                      device PD, transition to RTS (loopback is fine — it is
                      only used to execute local REG_MR/LOCAL_INV WRs, which do
                      not require a peer).
net/rds/ib_frmr.c   : rds_ib_post_reg_frmr()/rds_ib_post_inv() post on
                      rds_ibdev->fastreg_qp and poll rds_ibdev->fastreg_cq,
                      instead of ic->i_cm_id->qp.
```

Effect: `RDS_GET_MR` succeeds at any time, decoupled from data connections —
which is what a multi-peer collective workload (all-to-all) needs, since every
rank registers windows up front and is read/written by many peers.

> If your other agent already prototyped a proxy QP in an `*frmr*` file, this is
> the integration point; the provider needs no change to benefit — it just stops
> seeing registration failures.

---

## 4. (Optional) IB zero-copy `sendmsg`  — NOT used, can be ignored

Your report notes `MSG_ZEROCOPY` is rejected for the IB transport
(`net/rds/send.c` restricts `RDS_CMSG_ZCOPY_COOKIE` to TCP). **The provider does
not rely on this** — bulk zero-copy is achieved via RDMA (cookies), not via
zcopy sendmsg. No patch required. (If you ever wanted a zero-copy *eager* path,
you would lift that restriction and wire `RDS_CMSG_ZCOPY_COOKIE` +
`RDS_CMSG_ZCOPY_COMPLETION` errqueue reaping into `rds_send_eager()` — but RDMA
rendezvous already covers the large-message case better.)

---

## 5. Notification queue depth (tuning, optional)

The provider reaps `RDS_CMSG_RDMA_STATUS` notifications in `rds_progress.c`
using a control buffer sized for 16 notifies per `recvmsg`. With very high RMA
op concurrency you may want a deeper `rs_notify_queue`; this is a kernel-side
tunable in `net/rds/af_rds.c`/`rds.h` if you observe notification backpressure.
No correctness impact — the provider loops until `EAGAIN`.

---

## 6. Summary

| change | needed for | stock kernel? |
|--------|-----------|----------------|
| `user_token` + `RDS_RDMA_NOTIFY_ME` notify | RMA/rendezvous completion | **required** (you have it) |
| raise `RDS_MAX_MSG_SIZE` | bigger *eager* datagrams | optional (provider avoids it) |
| raise 256-page MR cap | app RMA windows > 1 MiB | needed only for big windows |
| **proxy QP for FRMR** | register before connect / all-to-all | strongly recommended |
| IB `MSG_ZEROCOPY` | — | not used |

Bring-up order: run on stock kernel → confirm eager + rendezvous + small RMA →
add proxy QP → (if needed) raise MR page cap for large windows.

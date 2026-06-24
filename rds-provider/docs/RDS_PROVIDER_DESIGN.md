# libfabric RDS provider — design & implementation

Author: generated for Gaurang Gupta's RDS-over-RDMA / Intel MPI work
Target: `prov/rds` in libfabric (ofiwg/libfabric), kernel RDS (`net/rds`)

---

## 1. Goal

Run Intel MPI 2021 (MPICH CH4 / OFI netmod) collectives — `alltoall`,
`allgather`, etc. — over the Linux kernel **RDS** (Reliable Datagram Sockets)
transport on RoCE v2, extracting the **zero-copy 1-sided RDMA** performance you
already measured with `rds_rdma_v2.c`:

| transport            | 2 B RTT | 1 MB RTT |
|----------------------|---------|----------|
| RDS TCP (copy)       | ~30 µs  | 487 µs   |
| RDS IB (copy/bounce) | ~41 µs  | 529 µs   |
| **RDS RDMA V2 (ZC)** | **14 µs** | **212 µs** |
| Native IB verbs RC   | 10 µs   | 184 µs   |

The UDP-based provider you tried earlier paid the kernel-copy tax on every
message (it behaves like the "RDS IB copy" / "RDS TCP" rows). This provider is
built specifically around the **RDS_GET_MR → cookie → RDS_CMSG_RDMA_ARGS**
mechanism so that bulk data is DMA'd directly into/out of the application's
pinned pages — no user↔kernel bounce copy — i.e. the "RDS RDMA V2" row, but
generalised to arbitrary peers and wired into libfabric's tagged + RMA API.

## 2. Why RDS maps cleanly onto `FI_EP_RDM`

RDS already provides exactly the semantics libfabric's reliable-datagram
endpoint needs, so we do **not** need `rxd`/`rxm` reliability shims on top:

| libfabric `FI_EP_RDM` needs | RDS provides natively |
|-----------------------------|------------------------|
| reliable delivery           | reliable (ACKed) |
| ordered per peer            | in-order per (src,dst) IP pair |
| message boundaries          | `SOCK_SEQPACKET` |
| connectionless addressing   | datagram API, connection setup is implicit/automatic in the kernel |
| 1-sided RMA                  | `RDS_GET_MR` cookies + `RDS_CMSG_RDMA_ARGS` |
| remote completion notify    | `RDS_RDMA_NOTIFY_ME` → `RDS_CMSG_RDMA_STATUS` |

This is the central design decision: **RDS is the reliability + RDMA layer; the
provider is a thin, zero-copy shim** that translates libfabric verbs into RDS
socket operations. Addressing is plain IPv4 (`FI_SOCKADDR_IN`) because an RDS
socket is bound to a local RoCE/IPoIB interface address.

## 3. Object model (built on `prov/util`)

Like the `udp` and `tcp` providers, we reuse libfabric's `util_*` helpers for
all the boilerplate objects and only hand-write the data path:

```
fi_fabric   -> util_fabric            (ofi_fabric_init)
fi_domain   -> rds_domain{util_domain}(ofi_domain_init)  + custom fi_ops_mr
fi_av       -> ofi_ip_av              (IP address vector, reverse lookup for FI_SOURCE)
fi_cq       -> util_cq                (ofi_cq_init + ofi_cq_progress)
fi_eq       -> util_eq               (ofi_eq_create)
fi_mr       -> rds_mr                 (RDS_GET_MR cookie == mr_key)        [custom]
fi_endpoint -> rds_ep{util_ep}        (one PF_RDS/SOCK_SEQPACKET socket)   [custom]
```

`rds_ep_progress()` is registered with `ofi_endpoint_init()` and is invoked by
`ofi_cq_progress()` on every `fi_cq_read()` — that is the single point where the
RDS socket is drained and all completions are produced.

### Source files

| file | role |
|------|------|
| `rds_init.c`     | provider struct, `fi_getinfo` (via `ofi_ip_getinfo`), params |
| `rds_attr.c`     | capability/attribute tables (`fi_info`) |
| `rds_fabric.c`   | fabric open/close |
| `rds_domain.c`   | domain open/close, installs `rds_mr_ops` |
| `rds_cq.c`       | CQ open (util_cq + `ofi_cq_progress`) |
| `rds_mr.c`       | **memory registration → cookie** (the zero-copy enabler) |
| `rds_ep.c`       | endpoint lifecycle, socket setup, `setname`/`getname`, bind/enable |
| `rds_msg.c`      | tagged/msg send+recv, **matching engine**, eager + rendezvous |
| `rds_rma.c`      | `fi_read`/`fi_write` over cookies |
| `rds_progress.c` | drain socket, dispatch inbound, reap RDMA completion notifies |

## 4. Capabilities advertised

`caps = FI_MSG | FI_TAGGED | FI_RMA | FI_SEND | FI_RECV | FI_READ | FI_WRITE |
FI_REMOTE_READ | FI_REMOTE_WRITE | FI_DIRECTED_RECV | FI_SOURCE`.

`ep_attr.type = FI_EP_RDM`, `addr_format = FI_SOCKADDR_IN`,
`max_msg_size = 256 MiB` (fragmented internally — see §7).

`mr_mode = FI_MR_ALLOCATED | FI_MR_PROV_KEY` (scalable/offset MR semantics):

- **`FI_MR_PROV_KEY`** — the provider mints the key, and the key *is the RDS
  cookie* returned by `RDS_GET_MR`. A peer that receives the key drops it
  straight into `rds_rdma_args.cookie`.
- **No `FI_MR_VIRT_ADDR`** — RMA target addresses are *offsets from the MR
  start*, which is exactly what `rds_rdma_args.remote_vec.addr` expects (the
  `rds_rdma_v2.c` benchmark proves `remote_vec.addr == 0` lands at the
  registered buffer start). The cookie's low 32 bits already carry the in-page
  offset, so unaligned buffers work too.
- **No `FI_MR_LOCAL`** — local RDMA buffers are pinned by the kernel on demand
  (`get_user_pages`) during each op, so the application never has to register
  send/recv buffers. This is why the path is genuinely zero-copy without
  burdening the app.

`atomics` are intentionally not advertised in v1 (RDS supports
`RDS_CMSG_ATOMIC_*`; see §10 future work). MPICH emulates MPI atomics over
RMA/active messages when the provider lacks `FI_ATOMIC`.

## 5. Memory registration — the cookie (rds_mr.c)

```
fi_mr_reg(buf, len, access)
  └─ setsockopt(sock, SOL_RDS, RDS_GET_MR, {vec={buf,len}, &cookie, RDS_RDMA_READWRITE})
       → kernel pins ≤256 pages, builds an FRMR, returns rds_rdma_cookie_t
  └─ mr->mr_fid.key = cookie ;  mr->mr_fid.mem_desc = mr
```

- The cookie (`r_key<<32 | page_offset`) is exported verbatim as `fi_mr_key()`.
- The provider keeps **no key→MR map**: an inbound RMA is validated entirely by
  the *target's* HCA against the r_key, so the initiator just forwards the
  cookie it was given.
- `RDS_GET_MR` is a *per-socket* op needing a socket bound to a local RoCE
  device. Domain-scoped `fi_mr_reg` therefore borrows the first **enabled**
  endpoint's socket (`rds_domain.reg_ep`, set in `FI_ENABLE`). Cookies are valid
  for that host's HCA PD regardless of which endpoint later issues the op, so a
  single registration serves all peers — important for all-to-all (one window,
  N readers/writers).
- Stock-kernel limit: 256 pages / 1 MiB per MR (`RDS_KERNEL_SEG_MAX`).
  Registrations larger than that fail here; the rendezvous path works around it
  by registering source buffers one ≤1 MiB chunk at a time (§7).

## 6. Eager path (small messages) — rds_msg.c

For payloads ≤ `eager_size` (default 8 KiB, `FI_RDS_EAGER_SIZE`):

```
send:  sendmsg(iov = [rds_hdr | user payload])           # one kernel copy in
       → completion written immediately (buffer reusable on return)

recv:  progress recvmsg(into bounce) → parse rds_hdr → match → copy out to app iov
```

This is the cheap path for short MPI traffic. There is one copy in and one copy
out (unavoidable for tiny messages and *cheaper* than RDMA setup at that size —
your data shows RDMA only wins decisively below ~1 KB because of busy-poll, and
copies dominate setup above), but no extra bounce: the kernel copy *is* the
transfer.

## 7. Rendezvous path (large messages) — the zero-copy send/recv

For payloads > `eager_size`, **receiver-driven RDMA-READ rendezvous**:

```
SENDER                                   RECEIVER
------                                    --------
register source buf in ≤1MiB chunks
  (RDS_GET_MR per chunk → cookies)
build RTS = [hdr | seg[]{cookie,len}]
sendmsg(RTS)  ───────────────────────▶   progress recvmsg → match posted trecv
defer send completion                    for each seg:
                                           fi_read-equivalent:
                                           sendmsg(RDS_CMSG_RDMA_ARGS,
                                                   cookie, NOTIFY_ME, token)
                                           → HCA DMAs directly into app buffer  ★ZERO COPY
                                         (reap RDS_CMSG_RDMA_STATUS per seg)
                                         when all segs done:
FIN ◀──────────────────────────────────   sendmsg(FIN, sender_id)
release source MRs                       write recv completion
write send completion
```

Why **read** rendezvous (receiver pulls) rather than **write** (sender pushes):
one round trip (RTS → READ → FIN), and the receiver — which knows the matched
destination buffer — drives the DMA straight into it. The application buffer is
never copied; the kernel pins it for the duration of the read. This is the
generalisation of `rds_rdma_v2.c` to MPI: instead of a pre-agreed cookie + last-
byte busy-poll, the cookie travels in the RTS and completion is an actual RDMA
notification.

**>1 MiB messages** are handled by splitting into ≤1 MiB segments (one MR +
one RDMA read each), reassembled at their natural offsets in the receive buffer
and completed once via a refcount (`seg_remaining`). This transparently lifts
the 1 MiB RDS ceiling for point-to-point — the application sees a single logical
message. (For app-level RMA windows >1 MiB the kernel patch in
`RDS_KERNEL_CHANGES.md` is still required, because a single `fi_mr_key` can only
carry one cookie.)

## 8. Matching engine (rds_msg.c)

Separate posted/unexpected queues for `FI_MSG` and `FI_TAGGED`:

- **Tagged**: `((rx_tag ^ msg_tag) & ~ignore) == 0` plus optional source match
  (`FI_DIRECTED_RECV`).
- **Msg**: FIFO, optional source match.
- Inbound with no match is buffered as `rds_unexp` (eager payload copied;
  RTS segment list stashed for a deferred read). A later post scans the
  unexpected queue first.

Ordering: RDS guarantees per-peer in-order arrival, so we honour `FI_ORDER_SAS`
(MPI tag matching depends on it). Completion order is `FI_ORDER_NONE` —
rendezvous completes asynchronously when the RDMA-read notification arrives, so
it may reorder vs. a later eager op. Each completion carries its own
`op_context`, which is all MPICH requires.

## 9. RMA path (rds_rma.c)

`fi_write`/`fi_read` → a single `sendmsg` carrying only an
`RDS_CMSG_RDMA_ARGS` control message (no payload), exactly like
`rds_rdma_v2.c`'s `rdma_write()` but with:

- `flags = RDS_RDMA_NOTIFY_ME | (write ? RDS_RDMA_READWRITE : 0)`
  (`READWRITE` set ⇒ RDMA write; clear ⇒ RDMA read, per the kernel's
  `op_write = !!(flags & RDS_RDMA_READWRITE)`).
- `user_token` = a provider op id. Completion is delivered later as
  `RDS_CMSG_RDMA_STATUS` (`struct rds_rdma_notify{user_token,status}`) on the
  initiator's recvmsg and matched back in `rds_handle_rdma_notify()`.

This gives real libfabric completion semantics instead of the benchmark's
last-byte busy-poll, while keeping the same zero-copy DMA.

`fi_inject` for RMA copies into a small private bounce (the source must be
reusable on return but the DMA may outlive the call), freed on notify; no
completion is generated.

## 10. Concurrency / locking

- All endpoint state (posted/unexpected/pending lists, the socket) is serialised
  by `util_ep.lock`.
- Completions go through `ofi_cq_write*` which take `cq_lock` internally.
- Lock partial order is acyclic: `cq.ep_list_lock → ep.lock → cq.cq_lock`.
  `ofi_cq_readfrom()` runs `progress()` *before* taking `cq_lock`, so progress
  (which takes `ep.lock` then `cq_lock`) never inverts against the read path.
- `data_progress = FI_PROGRESS_MANUAL`: progress happens inside `fi_cq_read`
  (MPICH busy-polls). No progress thread.

## 11. Known limitations & where they're addressed

| limitation | mitigation |
|------------|------------|
| 1 MiB per RDS datagram (copy path) | eager only used ≤ eager_size; large = rendezvous |
| 256 pages / 1 MiB per MR | rendezvous registers per ≤1 MiB chunk; app RMA windows >1 MiB need kernel patch (`RDS_KERNEL_CHANGES.md`) |
| `RDS_GET_MR` may need a live QP to post FRMR (no proxy QP) | rely on MPI init barrier to pre-connect; robust fix = proxy QP patch (`RDS_KERNEL_CHANGES.md` §3) |
| IB `MSG_ZEROCOPY` send unsupported by kernel | not used — bulk zero-copy is via RDMA, not zcopy sendmsg |
| no `FI_ATOMIC` | MPICH emulates; optional `RDS_CMSG_ATOMIC_*` mapping is future work |
| blocking `fi_cq_sread` won't wake on socket data | use `FI_WAIT_NONE` + busy-poll (MPI default); epoll wiring is future work |
| heterogeneous endianness | header is host-endian today (homogeneous x86 GCP VMs); add htonll/ntohll for mixed fabrics |

## 12. End-to-end data flow summary

```
MPI_Alltoall
  → MPICH CH4/OFI: many fi_tsend/fi_trecv (+ fi_read/fi_write for large/onesided)
    → small  : eager   sendmsg([hdr|data])              (1 copy each way)
    → large  : rendezvous RTS + RDMA READ               (ZERO COPY bulk)   ★
    → RMA    : RDS_CMSG_RDMA_ARGS                        (ZERO COPY)        ★
  → fi_cq_read → ofi_cq_progress → rds_ep_progress
       → recvmsg drains: eager deliver / RTS→read / FIN / RDMA_STATUS notify
```

The starred paths carry the bulk bytes with no user↔kernel copy, which is the
"RDS RDMA V2" performance you want, now usable by unmodified Intel MPI.

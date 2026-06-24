/*
 * libfabric RDS provider
 *
 * A native libfabric provider built directly on the Linux kernel RDS
 * (Reliable Datagram Sockets) socket family (PF_RDS / SOCK_SEQPACKET).
 *
 * Design goals (see docs/RDS_PROVIDER_DESIGN.md for the full writeup):
 *   - FI_EP_RDM endpoint with FI_MSG, FI_TAGGED and FI_RMA semantics, mapping
 *     1:1 onto RDS reliable, ordered, message-boundary-preserving datagrams.
 *   - True zero-copy for large transfers and for one-sided RMA by using the
 *     RDS RDMA cookie mechanism (RDS_GET_MR / RDS_CMSG_RDMA_ARGS). The kernel
 *     pins the user pages and the HCA DMAs directly into/out of them; there is
 *     no user<->kernel bounce copy on the bulk path.
 *   - Small messages travel "eager" (a single sendmsg carrying header+payload),
 *     which is the cheapest path for short MPI traffic.
 *   - Large messages travel "rendezvous": the sender registers its source
 *     buffer and ships only a tiny RTS descriptor; the receiver issues a
 *     one-sided RDMA READ straight into the matched application buffer. This is
 *     the zero-copy send/recv path requested for collectives (alltoall,
 *     allgather, ...).
 *
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 */

#ifndef _RDS_H_
#define _RDS_H_

#if HAVE_CONFIG_H
#  include <config.h>
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_tagged.h>

#include <ofi.h>
#include <ofi_enosys.h>
#include <ofi_list.h>
#include <ofi_mem.h>
#include <ofi_net.h>
#include <ofi_util.h>
#include <ofi_atom.h>

/*
 * The kernel RDS UAPI lives in <linux/rds.h>.  We pull it in for the structs
 * (rds_iovec, rds_get_mr_args, rds_rdma_args, rds_rdma_notify, ...) and the
 * rds_rdma_cookie_t type, then backfill any constants that may be missing on
 * older toolchains so the provider builds against a stable surface.
 */
#include <linux/rds.h>

#ifndef AF_RDS
#define AF_RDS		21
#endif
#ifndef PF_RDS
#define PF_RDS		AF_RDS
#endif
#ifndef SOL_RDS
#define SOL_RDS		276
#endif

#ifndef SO_RDS_TRANSPORT
#define SO_RDS_TRANSPORT	8
#endif
#ifndef RDS_TRANS_IB
#define RDS_TRANS_IB		0
#endif
#ifndef RDS_TRANS_TCP
#define RDS_TRANS_TCP		2
#endif

#ifndef RDS_GET_MR
#define RDS_GET_MR		2
#endif
#ifndef RDS_FREE_MR
#define RDS_FREE_MR		3
#endif
#ifndef RDS_RECVERR
#define RDS_RECVERR		5
#endif
#ifndef RDS_GET_MR_FOR_DEST
#define RDS_GET_MR_FOR_DEST	7
#endif

#ifndef RDS_CMSG_RDMA_ARGS
#define RDS_CMSG_RDMA_ARGS	1
#endif
#ifndef RDS_CMSG_RDMA_DEST
#define RDS_CMSG_RDMA_DEST	2
#endif
#ifndef RDS_CMSG_RDMA_MAP
#define RDS_CMSG_RDMA_MAP	3
#endif
#ifndef RDS_CMSG_RDMA_STATUS
#define RDS_CMSG_RDMA_STATUS	4
#endif

#ifndef RDS_RDMA_READWRITE
#define RDS_RDMA_READWRITE	0x0001
#endif
#ifndef RDS_RDMA_FENCE
#define RDS_RDMA_FENCE		0x0002
#endif
#ifndef RDS_RDMA_INVALIDATE
#define RDS_RDMA_INVALIDATE	0x0004
#endif
#ifndef RDS_RDMA_USE_ONCE
#define RDS_RDMA_USE_ONCE	0x0008
#endif
#ifndef RDS_RDMA_DONTWAIT
#define RDS_RDMA_DONTWAIT	0x0010
#endif
#ifndef RDS_RDMA_NOTIFY_ME
#define RDS_RDMA_NOTIFY_ME	0x0020
#endif
#ifndef RDS_RDMA_SILENT
#define RDS_RDMA_SILENT		0x0040
#endif


/* ------------------------------------------------------------------ */
/* Provider tunables						      */
/* ------------------------------------------------------------------ */

#define RDS_IOV_LIMIT		8

/*
 * Largest single RDS datagram / single RDMA op the stock kernel module will
 * accept.  Both the copy path (RDS_MAX_MSG_SIZE) and MR registration
 * (RDS_MAX_PAGES_PER_MR == 256 pages) cap out at 1 MiB.  If the kernel is
 * patched to lift these (see docs/RDS_KERNEL_CHANGES.md) this constant and
 * rds_info.ep_attr->max_msg_size can be raised in lockstep.
 */
#define RDS_KERNEL_SEG_MAX	(1024 * 1024)

/* Default eager threshold; <= this many payload bytes ship inline. Overridable
 * via FI_RDS_EAGER_SIZE / the "eager_size" param. */
#define RDS_DEF_EAGER_SIZE	8192
#define RDS_MAX_EAGER_SIZE	(RDS_KERNEL_SEG_MAX - (int) sizeof(struct rds_hdr))

/* Aggregate message ceiling we advertise. We fragment anything above one
 * kernel segment into multiple RDMA-read segments on the rendezvous path. */
#define RDS_MAX_MSG_SIZE	(256ULL * 1024 * 1024)

#define RDS_HDR_MAGIC		0x53445231u	/* "RDS1" */

#define RDS_PROV_NAME		"rds"


/* ------------------------------------------------------------------ */
/* Wire framing						      */
/* ------------------------------------------------------------------ */

enum rds_op {
	RDS_OP_EAGER	= 1,	/* header + inline payload, copy-in/out      */
	RDS_OP_RTS	= 2,	/* rendezvous request; payload = seg array   */
	RDS_OP_FIN	= 3,	/* rendezvous done; receiver -> sender ack   */
};

enum {
	RDS_HF_TAGGED	= 1 << 0,	/* tagged (else plain FI_MSG)        */
	RDS_HF_CQ_DATA	= 1 << 1,	/* hdr.data carries FI_REMOTE_CQ_DATA */
};

/*
 * Every RDS datagram we put on the wire starts with this fixed header.  It is
 * sent as the first element of the sendmsg iovec so the payload that follows is
 * contiguous and copy-friendly on the receive side.  All fields little/host
 * endian: RDS today is only used between homogeneous x86 GCP VMs; the design
 * doc notes htonll/ntohll hooks for a heterogeneous fabric.
 */
struct rds_hdr {
	uint32_t	magic;
	uint8_t		op;		/* enum rds_op            */
	uint8_t		flags;		/* RDS_HF_*               */
	uint16_t	seg_cnt;	/* rendezvous: # segments */
	uint64_t	tag;		/* FI_TAGGED tag          */
	uint64_t	data;		/* FI_REMOTE_CQ_DATA      */
	uint64_t	size;		/* total payload bytes    */
	uint64_t	id;		/* sender op id; echoed in FIN / used as token */
};

/* One rendezvous source segment, ferried as the RTS payload. */
struct rds_rndzv_seg {
	uint64_t	cookie;		/* sender source-buffer RDS cookie (rkey) */
	uint64_t	off;		/* offset within that MR (usually 0)      */
	uint64_t	len;		/* bytes in this segment (<= seg_max)     */
};


/* ------------------------------------------------------------------ */
/* Provider objects						      */
/* ------------------------------------------------------------------ */

extern struct fi_provider rds_prov;
extern struct util_prov rds_util_prov;
extern struct fi_info rds_info;

extern size_t rds_eager_size;		/* resolved at init from the param */

int rds_fabric(struct fi_fabric_attr *attr, struct fid_fabric **fabric,
	       void *context);
int rds_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		    struct fid_domain **dom, void *context);
int rds_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
		struct fid_cq **cq_fid, void *context);
int rds_endpoint(struct fid_domain *domain, struct fi_info *info,
		 struct fid_ep **ep_fid, void *context);

/* fi_ops_mr table installed on the domain */
extern struct fi_ops_mr rds_mr_ops;


/*
 * struct rds_mr - a registered memory region.
 *
 * fi_mr_reg() asks the kernel (RDS_GET_MR) to pin the pages and hand back a
 * cookie.  We export that cookie verbatim as the libfabric mr_key, so a peer
 * that receives the key can drop it straight into rds_rdma_args.cookie.  No
 * provider-side key map is needed: the kernel validates the cookie on the
 * target during the RDMA op.
 */
struct rds_mr {
	struct fid_mr		mr_fid;
	struct rds_domain	*domain;
	struct rds_ep		*ep;		/* socket the MR was created on */
	rds_rdma_cookie_t	cookie;
	uint64_t		offset;		/* page offset baked into cookie */
	void			*buf;
	size_t			len;
	uint64_t		access;
	ofi_atomic32_t		ref;		/* rendezvous-tx may share it   */
};

struct rds_domain {
	struct util_domain	util_domain;
	/*
	 * The endpoint whose RDS socket is used to service domain-scoped
	 * fi_mr_reg() calls.  RDS_GET_MR is a per-socket operation that needs a
	 * socket bound to a local RoCE device, so we borrow the first enabled
	 * endpoint's socket.  Cookies it returns are valid for that host's HCA
	 * regardless of which endpoint later issues the RDMA op.
	 */
	struct rds_ep		*reg_ep;
};

/* RDS cookie packing (kernel-internal layout, stable UAPI behaviour). */
static inline rds_rdma_cookie_t rds_make_cookie(uint32_t r_key, uint32_t off)
{
	return ((rds_rdma_cookie_t) r_key << 32) | off;
}
static inline uint32_t rds_cookie_key(rds_rdma_cookie_t c)
{
	return (uint32_t) (c >> 32);
}
static inline uint32_t rds_cookie_off(rds_rdma_cookie_t c)
{
	return (uint32_t) (c & 0xffffffff);
}


/* ------------------------------------------------------------------ */
/* Receive side bookkeeping					      */
/* ------------------------------------------------------------------ */

/* An application-posted receive (FI_MSG or FI_TAGGED). */
struct rds_rx_entry {
	struct dlist_entry	entry;
	void			*context;
	struct iovec		iov[RDS_IOV_LIMIT];
	size_t			iov_count;
	size_t			total_len;
	uint64_t		tag;
	uint64_t		ignore;
	fi_addr_t		addr;		/* directed recv, or FI_ADDR_UNSPEC */
	uint64_t		flags;		/* FI_TAGGED, FI_COMPLETION, ...    */
};

/* An arrived message that had no matching posted receive yet. */
struct rds_unexp {
	struct dlist_entry	entry;
	struct rds_hdr		hdr;
	fi_addr_t		addr;		/* resolved source fi_addr   */
	union ofi_sock_ip	src;		/* raw source for the FIN/read */
	size_t			seg_cnt;
	struct rds_rndzv_seg	*segs;		/* RTS: kept for deferred read */
	size_t			len;		/* EAGER: payload bytes        */
	char			data[];		/* EAGER: copied payload       */
};

/*
 * struct rds_pending - an operation whose completion is deferred.
 *
 *   RDS_PEND_RNDZV_TX : a large send; waits for the FIN ack from the receiver.
 *   RDS_PEND_RNDZV_RX : a large recv; waits for all of its RDMA-read segments
 *                       to be acknowledged via RDS_CMSG_RDMA_STATUS.
 *   RDS_PEND_RMA      : an fi_read/fi_write; waits for its RDMA_STATUS notify.
 */
enum rds_pend_type {
	RDS_PEND_RNDZV_TX = 1,
	RDS_PEND_RNDZV_RX,
	RDS_PEND_RMA,
};

struct rds_pending {
	struct dlist_entry	entry;
	uint64_t		id;		/* token used to find it again */
	enum rds_pend_type	type;
	void			*context;
	uint64_t		flags;		/* CQ completion flags         */
	size_t			len;
	fi_addr_t		addr;
	uint64_t		data;		/* remote CQ data (rx)         */
	uint64_t		tag;
	int			error;		/* first failure seen          */

	/* rendezvous accounting */
	uint32_t		seg_remaining;	/* RX: reads still outstanding */
	uint32_t		seg_total;
	union ofi_sock_ip	peer;		/* RX: where to send FIN       */
	struct rds_mr		**mrs;		/* TX: source MRs to release   */
	uint32_t		mr_cnt;

	uint64_t		peer_id;	/* RX: sender's id to echo in FIN */
	void			*bounce;	/* RMA inject temp buffer      */
	uint8_t			no_comp;	/* suppress the CQ entry       */
};


/* ------------------------------------------------------------------ */
/* Endpoint							      */
/* ------------------------------------------------------------------ */

struct rds_ep {
	struct util_ep		util_ep;
	int			sock;		/* PF_RDS / SOCK_SEQPACKET     */
	int			is_bound;
	int			transport;	/* RDS_TRANS_IB by default     */

	/* All of the below are serialised by util_ep.lock (the ep progress
	 * lock), taken in the data-path entry points and in progress. */
	struct dlist_entry	rx_posted_msg;	/* struct rds_rx_entry        */
	struct dlist_entry	rx_posted_tag;
	struct dlist_entry	rx_unexp_msg;	/* struct rds_unexp           */
	struct dlist_entry	rx_unexp_tag;
	struct dlist_entry	pending;	/* struct rds_pending         */

	uint64_t		op_id;		/* monotonic token allocator  */

	char			*rx_buf;	/* bounce buffer for recvmsg  */
	size_t			rx_buf_size;
	void			*cmsg_buf;	/* control buffer for recvmsg */
	size_t			cmsg_size;

	ofi_atomic32_t		ref;
};

/* msg/rma op tables and helpers, defined across rds_msg.c / rds_rma.c */
extern struct fi_ops_msg rds_msg_ops;
extern struct fi_ops_tagged rds_tagged_ops;
extern struct fi_ops_rma rds_rma_ops;

void rds_ep_progress(struct util_ep *util_ep);

/* internal helpers shared between data-path files */
struct rds_mr *rds_reg_internal(struct rds_ep *ep, const void *buf, size_t len,
				uint64_t access);
void rds_mr_put(struct rds_mr *mr);

ssize_t rds_post_recv(struct rds_ep *ep, const struct iovec *iov,
		      size_t iov_count, fi_addr_t addr, uint64_t tag,
		      uint64_t ignore, void *context, uint64_t flags,
		      int tagged);

ssize_t rds_generic_send(struct rds_ep *ep, const struct iovec *iov,
			 size_t iov_count, fi_addr_t dest_addr, uint64_t tag,
			 uint64_t data, void *context, uint64_t flags,
			 int tagged);

/*
 * Low-level one-sided RDMA op shared by fi_read/fi_write and by the rendezvous
 * receive path.  Builds rds_rdma_args (NOTIFY_ME + user_token=@token) and pushes
 * it with sendmsg; completion arrives later as RDS_CMSG_RDMA_STATUS.
 * @is_write != 0 issues an RDMA WRITE (local->remote), else an RDMA READ.
 */
ssize_t rds_post_rdma(struct rds_ep *ep, const void *dest_addr,
		      size_t dest_addrlen, rds_rdma_cookie_t cookie,
		      uint64_t remote_off, const struct iovec *local_iov,
		      size_t iov_cnt, int is_write, uint64_t token);

/* Inbound dispatch, called from the progress engine. */
void rds_handle_inbound(struct rds_ep *ep, struct rds_hdr *hdr, void *payload,
			size_t payload_len, const union ofi_sock_ip *src,
			fi_addr_t src_fi);
void rds_handle_rdma_notify(struct rds_ep *ep, uint64_t token, int status);

/* Slice [off, off+len) of @src into @dst (capped at RDS_IOV_LIMIT entries). */
size_t rds_iov_slice(struct iovec *dst, size_t *dst_cnt,
		     const struct iovec *src, size_t src_cnt, size_t off,
		     size_t len);

/* completion helpers */
void rds_cq_write_tx(struct rds_ep *ep, void *context, uint64_t flags,
		     size_t len, int err);

/* deferred-completion (pending) bookkeeping, shared by msg/rma/progress */
struct rds_pending *rds_pending_alloc(struct rds_ep *ep,
				      enum rds_pend_type type, void *context,
				      uint64_t flags);
struct rds_pending *rds_pending_find(struct rds_ep *ep, uint64_t id);
void rds_pending_complete(struct rds_ep *ep, struct rds_pending *pend);


/* ------------------------------------------------------------------ */
/* small helpers						      */
/* ------------------------------------------------------------------ */

static inline uint64_t rds_next_id(struct rds_ep *ep)
{
	/* id 0 is reserved as "none" */
	return ++ep->op_id ? ep->op_id : ++ep->op_id;
}

static inline size_t rds_iov_len(const struct iovec *iov, size_t count)
{
	size_t i, len = 0;
	for (i = 0; i < count; i++)
		len += iov[i].iov_len;
	return len;
}

static inline const void *
rds_av_addr(struct rds_ep *ep, fi_addr_t fi_addr)
{
	return ofi_ip_av_get_addr(ep->util_ep.av, fi_addr);
}

#endif /* _RDS_H_ */

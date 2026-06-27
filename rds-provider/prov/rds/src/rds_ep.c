/*
 * libfabric RDS provider - endpoint object.
 *
 * One FI_EP_RDM endpoint == one PF_RDS / SOCK_SEQPACKET socket forced onto the
 * IB (RoCE) transport.  All point-to-point, tagged and RMA traffic for the
 * endpoint flows through this socket.  RDS itself provides the reliability,
 * ordering and connection management, so there is no CM state machine here:
 * addresses are plain IP (sockaddr_in) exchanged out-of-band (MPI PMI) and
 * inserted into the AV.
 *
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 */

#include <stdlib.h>
#include <unistd.h>
#include "rds.h"

/* Largest RTS descriptor payload we could ever receive (one seg per kernel
 * segment of the biggest advertised message). */
#define RDS_RTS_MAX_PAYLOAD						\
	(((RDS_MAX_MSG_SIZE / RDS_KERNEL_SEG_MAX) + 1) *		\
	 sizeof(struct rds_rndzv_seg))

static int rds_setname(fid_t fid, void *addr, size_t addrlen)
{
	struct rds_ep *ep;

	ep = container_of(fid, struct rds_ep, util_ep.ep_fid.fid);
	if (bind(ep->sock, addr, (socklen_t) addrlen)) {
		FI_WARN(&rds_prov, FI_LOG_EP_CTRL, "bind failed: %s\n",
			strerror(errno));
		return -errno;
	}
	ep->is_bound = 1;
	return 0;
}

static int rds_getname(fid_t fid, void *addr, size_t *addrlen)
{
	struct rds_ep *ep;
	size_t buflen = *addrlen;

	ep = container_of(fid, struct rds_ep, util_ep.ep_fid.fid);
	if (ofi_getsockname(ep->sock, addr, (socklen_t *) addrlen))
		return -ofi_sockerr();

	return buflen < *addrlen ? -FI_ETOOSMALL : 0;
}

static struct fi_ops_cm rds_cm_ops = {
	.size		= sizeof(struct fi_ops_cm),
	.setname	= rds_setname,
	.getname	= rds_getname,
	.getpeer	= fi_no_getpeer,
	.connect	= fi_no_connect,
	.listen		= fi_no_listen,
	.accept		= fi_no_accept,
	.reject		= fi_no_reject,
	.shutdown	= fi_no_shutdown,
	.join		= fi_no_join,
};

static int rds_ep_getopt(fid_t fid, int level, int optname, void *optval,
			 size_t *optlen)
{
	return -FI_ENOPROTOOPT;
}

static int rds_ep_setopt(fid_t fid, int level, int optname, const void *optval,
			 size_t optlen)
{
	/*
	 * Intel MPI calls fi_setopt during init (e.g. FI_OPT_MIN_MULTI_RECV,
	 * FI_OPT_CUDA_API_PERMITTED).  We accept silently: none of these change
	 * the RDS data path, and returning -FI_ENOPROTOOPT aborts MPI_Init.
	 */
	return 0;
}

static ssize_t rds_ep_cancel(fid_t fid, void *context)
{
	struct rds_ep *ep;
	struct rds_rx_entry *rx;
	struct dlist_entry *item, *tmp;
	struct dlist_entry *lists[2];
	int i;

	ep = container_of(fid, struct rds_ep, util_ep.ep_fid.fid);
	lists[0] = &ep->rx_posted_msg;
	lists[1] = &ep->rx_posted_tag;

	ofi_genlock_lock(&ep->util_ep.lock);
	for (i = 0; i < 2; i++) {
		dlist_foreach_safe(lists[i], item, tmp) {
			rx = container_of(item, struct rds_rx_entry, entry);
			if (rx->context != context)
				continue;
			dlist_remove(&rx->entry);
			ofi_genlock_unlock(&ep->util_ep.lock);

			if (ep->util_ep.rx_cq) {
				struct fi_cq_err_entry err = {0};
				err.op_context = context;
				err.flags = FI_RECV |
					(rx->flags & FI_TAGGED ? FI_TAGGED : FI_MSG);
				err.tag = rx->tag;
				err.err = FI_ECANCELED;
				err.prov_errno = 0;
				ofi_cq_write_error(ep->util_ep.rx_cq, &err);
			}
			free(rx);
			return 0;
		}
	}
	ofi_genlock_unlock(&ep->util_ep.lock);
	return -FI_ENOENT;
}

/*
 * Stub atomic ops.  We advertise FI_ATOMICS so Intel MPI's getinfo accepts the
 * provider, but atomics are not implemented (run with
 * MPIR_CVAR_CH4_OFI_ENABLE_ATOMICS=0).  Pointing ep->atomic at no-op stubs that
 * return -FI_ENOSYS is safer than leaving it NULL: a stray fi_atomic gets a
 * clean error instead of a segfault, and fi_query_atomic reports unsupported.
 */
static struct fi_ops_atomic rds_atomic_ops = {
	.size		= sizeof(struct fi_ops_atomic),
	.write		= fi_no_atomic_write,
	.writev		= fi_no_atomic_writev,
	.writemsg	= fi_no_atomic_writemsg,
	.inject		= fi_no_atomic_inject,
	.readwrite	= fi_no_atomic_readwrite,
	.readwritev	= fi_no_atomic_readwritev,
	.readwritemsg	= fi_no_atomic_readwritemsg,
	.compwrite	= fi_no_atomic_compwrite,
	.compwritev	= fi_no_atomic_compwritev,
	.compwritemsg	= fi_no_atomic_compwritemsg,
	.writevalid	= fi_no_atomic_writevalid,
	.readwritevalid	= fi_no_atomic_readwritevalid,
	.compwritevalid	= fi_no_atomic_compwritevalid,
};

static struct fi_ops_ep rds_ep_ops = {
	.size		= sizeof(struct fi_ops_ep),
	.cancel		= rds_ep_cancel,
	.getopt		= rds_ep_getopt,
	.setopt		= rds_ep_setopt,
	.tx_ctx		= fi_no_tx_ctx,
	.rx_ctx		= fi_no_rx_ctx,
	.rx_size_left	= fi_no_rx_size_left,
	.tx_size_left	= fi_no_tx_size_left,
};

static int rds_ep_bind(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	struct rds_ep *ep;

	ep = container_of(fid, struct rds_ep, util_ep.ep_fid.fid);
	return ofi_ep_bind(&ep->util_ep, bfid, flags);
}

static int rds_ep_ctrl(struct fid *fid, int command, void *arg)
{
	struct rds_ep *ep;
	struct rds_domain *domain;

	ep = container_of(fid, struct rds_ep, util_ep.ep_fid.fid);

	switch (command) {
	case FI_ENABLE:
		if ((ofi_needs_rx(ep->util_ep.caps) && !ep->util_ep.rx_cq) ||
		    (ofi_needs_tx(ep->util_ep.caps) && !ep->util_ep.tx_cq))
			return -FI_ENOCQ;
		if (!ep->util_ep.av)
			return -FI_ENOAV;
		if (!ep->is_bound) {
			FI_WARN(&rds_prov, FI_LOG_EP_CTRL,
				"endpoint enabled without a bound source "
				"address; bind via fi_setname / src_addr\n");
			return -FI_EOPBADSTATE;
		}

		/* Lend our socket to the domain for fi_mr_reg(). */
		domain = container_of(ep->util_ep.domain, struct rds_domain,
				      util_domain);
		if (!domain->reg_ep)
			domain->reg_ep = ep;
		break;
	default:
		return -FI_ENOSYS;
	}
	return 0;
}

static void rds_ep_flush_lists(struct rds_ep *ep)
{
	struct dlist_entry *item, *tmp;
	struct rds_rx_entry *rx;
	struct rds_unexp *ux;
	struct rds_pending *pend;
	struct dlist_entry *rx_lists[2] = {
		&ep->rx_posted_msg, &ep->rx_posted_tag };
	struct dlist_entry *ux_lists[2] = {
		&ep->rx_unexp_msg, &ep->rx_unexp_tag };
	uint32_t i;
	int l;

	for (l = 0; l < 2; l++) {
		dlist_foreach_safe(rx_lists[l], item, tmp) {
			rx = container_of(item, struct rds_rx_entry, entry);
			dlist_remove(&rx->entry);
			free(rx);
		}
		dlist_foreach_safe(ux_lists[l], item, tmp) {
			ux = container_of(item, struct rds_unexp, entry);
			dlist_remove(&ux->entry);
			free(ux->segs);
			free(ux);
		}
	}

	dlist_foreach_safe(&ep->pending, item, tmp) {
		pend = container_of(item, struct rds_pending, entry);
		dlist_remove(&pend->entry);
		if (pend->cmrs) {
			struct rds_domain *d = container_of(ep->util_ep.domain,
						struct rds_domain, util_domain);
			for (i = 0; i < pend->mr_cnt; i++)
				rds_reg_cache_put(d, pend->cmrs[i]);
			free(pend->cmrs);
		} else if (pend->mrs) {
			for (i = 0; i < pend->mr_cnt; i++)
				rds_mr_put(pend->mrs[i]);
			free(pend->mrs);
		}
		free(pend->bounce);
		free(pend);
	}
}

static int rds_ep_close(struct fid *fid)
{
	struct rds_ep *ep;
	struct rds_domain *domain;

	ep = container_of(fid, struct rds_ep, util_ep.ep_fid.fid);
	if (ofi_atomic_get32(&ep->ref)) {
		FI_WARN(&rds_prov, FI_LOG_EP_CTRL, "endpoint busy\n");
		return -FI_EBUSY;
	}

	domain = container_of(ep->util_ep.domain, struct rds_domain,
			      util_domain);
	if (domain->reg_ep == ep)
		domain->reg_ep = NULL;

	rds_ep_ring_cleanup(ep);
	rds_ep_flush_deferred(ep);
	rds_ep_flush_lists(ep);

	if (ep->sock >= 0)
		ofi_close_socket(ep->sock);
	free(ep->rx_buf);
	free(ep->cmsg_buf);
	ofi_endpoint_close(&ep->util_ep);
	free(ep);
	return 0;
}

static struct fi_ops rds_ep_fi_ops = {
	.size		= sizeof(struct fi_ops),
	.close		= rds_ep_close,
	.bind		= rds_ep_bind,
	.control	= rds_ep_ctrl,
	.ops_open	= fi_no_ops_open,
};

static int rds_ep_init_socket(struct rds_ep *ep, struct fi_info *info)
{
	int transport = RDS_TRANS_IB;
	int family;
	int ret;

	family = info->src_addr ?
		 ((struct sockaddr *) info->src_addr)->sa_family : AF_INET;

	ep->sock = socket(PF_RDS, SOCK_SEQPACKET, 0);
	if (ep->sock < 0) {
		FI_WARN(&rds_prov, FI_LOG_EP_CTRL,
			"socket(PF_RDS) failed: %s. Is the rds module loaded?\n",
			strerror(errno));
		return -errno;
	}
	(void) family;

	/* Force the IB/RoCE transport: that is the zero-copy RDMA path. */
	ep->transport = transport;
	if (setsockopt(ep->sock, SOL_RDS, SO_RDS_TRANSPORT, &transport,
		       sizeof(transport))) {
		FI_WARN(&rds_prov, FI_LOG_EP_CTRL,
			"SO_RDS_TRANSPORT(IB) failed: %s\n", strerror(errno));
		ret = -errno;
		goto err;
	}

	/*
	 * Optional socket-buffer sizing.  Larger buffers let more datagrams be
	 * in flight before sendmsg returns EAGAIN, smoothing the bursts a
	 * many-to-many collective generates on the single RDS connection shared
	 * by a node pair.  Best-effort: the kernel clamps to net.core.*mem_max
	 * and we ignore failures.  Default 0 leaves the kernel value untouched.
	 */
	if (rds_sndbuf) {
		int v = (int) rds_sndbuf;
		if (setsockopt(ep->sock, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)))
			FI_INFO(&rds_prov, FI_LOG_EP_CTRL,
				"SO_SNDBUF=%d rejected: %s\n", v,
				strerror(errno));
	}
	if (rds_rcvbuf) {
		int v = (int) rds_rcvbuf;
		if (setsockopt(ep->sock, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v)))
			FI_INFO(&rds_prov, FI_LOG_EP_CTRL,
				"SO_RCVBUF=%d rejected: %s\n", v,
				strerror(errno));
	}

	if (info->src_addr) {
		ret = rds_setname(&ep->util_ep.ep_fid.fid, info->src_addr,
				  info->src_addrlen);
		if (ret)
			goto err;
	}

	ret = fi_fd_nonblock(ep->sock);
	if (ret)
		goto err;

	return 0;
err:
	ofi_close_socket(ep->sock);
	ep->sock = -1;
	return ret;
}

int rds_endpoint(struct fid_domain *domain, struct fi_info *info,
		 struct fid_ep **ep_fid, void *context)
{
	struct rds_ep *ep;
	size_t payload;
	int ret;

	ep = calloc(1, sizeof(*ep));
	if (!ep)
		return -FI_ENOMEM;

	ep->sock = -1;
	ofi_atomic_initialize32(&ep->ref, 0);
	dlist_init(&ep->rx_posted_msg);
	dlist_init(&ep->rx_posted_tag);
	dlist_init(&ep->rx_unexp_msg);
	dlist_init(&ep->rx_unexp_tag);
	dlist_init(&ep->pending);
	dlist_init(&ep->deferred);
	rds_ep_ring_init(ep);

	ret = ofi_endpoint_init(domain, &rds_util_prov, info, &ep->util_ep,
				context, rds_ep_progress);
	if (ret)
		goto err1;

	/* Receive bounce buffer: must hold a header plus either an eager
	 * payload or the largest possible RTS segment list. */
	payload = rds_eager_size > RDS_RTS_MAX_PAYLOAD ?
		  rds_eager_size : RDS_RTS_MAX_PAYLOAD;
	ep->rx_buf_size = sizeof(struct rds_hdr) + payload;
	ep->rx_buf = malloc(ep->rx_buf_size);
	if (!ep->rx_buf) {
		ret = -FI_ENOMEM;
		goto err2;
	}

	/* Control buffer big enough for a batch of RDMA completion notifies.
	 * Dense rendezvous collectives retire many RDMA reads at once; a deeper
	 * batch drains them in fewer recvmsg calls (RDS leaves any overflow
	 * queued for the next call, so this is throughput, not correctness). */
	ep->cmsg_size = CMSG_SPACE(sizeof(struct rds_rdma_notify)) * 64;
	ep->cmsg_buf = malloc(ep->cmsg_size);
	if (!ep->cmsg_buf) {
		ret = -FI_ENOMEM;
		goto err3;
	}

	ret = rds_ep_init_socket(ep, info);
	if (ret)
		goto err4;

	*ep_fid = &ep->util_ep.ep_fid;
	(*ep_fid)->fid.ops = &rds_ep_fi_ops;
	(*ep_fid)->ops = &rds_ep_ops;
	(*ep_fid)->cm = &rds_cm_ops;
	(*ep_fid)->msg = &rds_msg_ops;
	(*ep_fid)->tagged = &rds_tagged_ops;
	(*ep_fid)->rma = &rds_rma_ops;
	(*ep_fid)->atomic = &rds_atomic_ops;
	return 0;

err4:
	free(ep->cmsg_buf);
err3:
	free(ep->rx_buf);
err2:
	ofi_endpoint_close(&ep->util_ep);
err1:
	free(ep);
	return ret;
}

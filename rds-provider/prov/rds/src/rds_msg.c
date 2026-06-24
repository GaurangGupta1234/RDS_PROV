/*
 * libfabric RDS provider - tagged/message data path + matching engine.
 *
 * Two transfer protocols:
 *
 *   EAGER (payload <= eager_size): a single sendmsg ships [header | payload].
 *   The kernel copies it once; the buffer is reusable immediately so the send
 *   completes synchronously.  The receiver copies the payload out of its bounce
 *   buffer into the matched application buffer.  Cheapest path for small MPI
 *   traffic.
 *
 *   RENDEZVOUS (payload > eager_size): the sender registers its source
 *   buffer(s) and ships only a tiny RTS descriptor (header + an array of
 *   {cookie,len} segments).  The receiver matches the RTS to a posted receive
 *   and issues one-sided RDMA READs straight into the application buffer -- a
 *   true zero-copy receive.  When the reads complete the receiver sends a FIN
 *   so the sender can release its source registration and complete its send.
 *
 * All list/queue manipulation here runs under ep->util_ep.lock, taken by the
 * data-path entry points and by rds_ep_progress().
 *
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 */

#include <stdlib.h>
#include "rds.h"
#include <ofi_iov.h>

/* ------------------------------------------------------------------ */
/* completion writers						      */
/* ------------------------------------------------------------------ */

void rds_cq_write_tx(struct rds_ep *ep, void *context, uint64_t flags,
		     size_t len, int err)
{
	struct util_cq *cq = ep->util_ep.tx_cq;

	if (!cq)
		return;

	if (err) {
		struct fi_cq_err_entry e = {0};
		e.op_context = context;
		e.flags = flags;
		e.len = len;
		e.err = err < 0 ? -err : err;
		ofi_cq_write_error(cq, &e);
	} else {
		ofi_cq_write(cq, context, flags, len, NULL, 0, 0);
	}
}

static void rds_cq_write_rx(struct rds_ep *ep, void *context, uint64_t flags,
			    size_t len, uint64_t data, uint64_t tag,
			    fi_addr_t src, int err)
{
	struct util_cq *cq = ep->util_ep.rx_cq;

	if (!cq)
		return;

	if (err) {
		struct fi_cq_err_entry e = {0};
		e.op_context = context;
		e.flags = flags;
		e.len = len;
		e.data = data;
		e.tag = tag;
		e.err = err < 0 ? -err : err;
		ofi_cq_write_error(cq, &e);
	} else if (ep->util_ep.caps & FI_SOURCE) {
		ofi_cq_write_src(cq, context, flags, len, NULL, data, tag, src);
	} else {
		ofi_cq_write(cq, context, flags, len, NULL, data, tag);
	}
}

/* ------------------------------------------------------------------ */
/* pending (deferred-completion) bookkeeping			      */
/* ------------------------------------------------------------------ */

struct rds_pending *rds_pending_alloc(struct rds_ep *ep,
				      enum rds_pend_type type, void *context,
				      uint64_t flags)
{
	struct rds_pending *p;

	p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;

	p->id = rds_next_id(ep);
	p->type = type;
	p->context = context;
	p->flags = flags;
	dlist_insert_tail(&p->entry, &ep->pending);
	return p;
}

struct rds_pending *rds_pending_find(struct rds_ep *ep, uint64_t id)
{
	struct rds_pending *p;
	struct dlist_entry *item;

	dlist_foreach(&ep->pending, item) {
		p = container_of(item, struct rds_pending, entry);
		if (p->id == id)
			return p;
	}
	return NULL;
}

/* Generic terminal completion: used by RMA ops and by rendezvous-tx on FIN. */
void rds_pending_complete(struct rds_ep *ep, struct rds_pending *pend)
{
	uint32_t i;

	if (pend->type == RDS_PEND_RNDZV_TX || pend->type == RDS_PEND_RMA) {
		if (!pend->no_comp)
			rds_cq_write_tx(ep, pend->context, pend->flags,
					pend->len, pend->error);
	}

	for (i = 0; i < pend->mr_cnt; i++)
		rds_mr_put(pend->mrs[i]);
	free(pend->mrs);
	free(pend->bounce);
	dlist_remove(&pend->entry);
	free(pend);
}

/* ------------------------------------------------------------------ */
/* wire helpers						      */
/* ------------------------------------------------------------------ */

static void rds_send_fin(struct rds_ep *ep, const union ofi_sock_ip *peer,
			 uint64_t id)
{
	struct rds_hdr hdr;
	struct iovec iov;
	struct msghdr msg;

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = RDS_HDR_MAGIC;
	hdr.op = RDS_OP_FIN;
	hdr.id = id;

	iov.iov_base = &hdr;
	iov.iov_len = sizeof(hdr);

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = (void *) peer;
	msg.msg_namelen = ofi_sizeofaddr((const struct sockaddr *) peer);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	/* Best effort: if this is dropped the sender leaks one registration
	 * until ep close. It should not be dropped on a reliable transport. */
	(void) sendmsg(ep->sock, &msg, 0);
}

/* Slice [off, off+len) of a source iov array into a destination iov array. */
size_t rds_iov_slice(struct iovec *dst, size_t *dst_cnt,
		     const struct iovec *src, size_t src_cnt,
		     size_t off, size_t len)
{
	size_t i, pos = 0, dn = 0, got = 0;

	for (i = 0; i < src_cnt && len && dn < RDS_IOV_LIMIT; i++) {
		size_t blen = src[i].iov_len;
		size_t s, take;

		if (pos + blen <= off) {
			pos += blen;
			continue;
		}
		s = (off > pos) ? off - pos : 0;
		take = MIN(blen - s, len);
		dst[dn].iov_base = (char *) src[i].iov_base + s;
		dst[dn].iov_len = take;
		dn++;
		got += take;
		len -= take;
		pos += blen;
	}
	*dst_cnt = dn;
	return got;
}

/* ------------------------------------------------------------------ */
/* matching						      */
/* ------------------------------------------------------------------ */

static int rds_tag_match(uint64_t rx_tag, uint64_t ignore, uint64_t msg_tag)
{
	return ((rx_tag ^ msg_tag) & ~ignore) == 0;
}

static int rds_addr_match(fi_addr_t rx_addr, fi_addr_t src)
{
	return rx_addr == FI_ADDR_UNSPEC || rx_addr == src;
}

/* Find and remove a posted receive matching an arrived message. */
static struct rds_rx_entry *rds_match_posted(struct rds_ep *ep,
					     struct rds_hdr *hdr,
					     fi_addr_t src, int tagged)
{
	struct dlist_entry *list, *item;
	struct rds_rx_entry *rx;

	list = tagged ? &ep->rx_posted_tag : &ep->rx_posted_msg;
	dlist_foreach(list, item) {
		rx = container_of(item, struct rds_rx_entry, entry);
		if (!rds_addr_match(rx->addr, src))
			continue;
		if (tagged && !rds_tag_match(rx->tag, rx->ignore, hdr->tag))
			continue;
		dlist_remove(&rx->entry);
		return rx;
	}
	return NULL;
}

/* Find and remove an unexpected message matching a freshly posted receive. */
static struct rds_unexp *rds_match_unexp(struct rds_ep *ep, fi_addr_t addr,
					 uint64_t tag, uint64_t ignore,
					 int tagged)
{
	struct dlist_entry *list, *item;
	struct rds_unexp *ux;

	list = tagged ? &ep->rx_unexp_tag : &ep->rx_unexp_msg;
	dlist_foreach(list, item) {
		ux = container_of(item, struct rds_unexp, entry);
		if (!rds_addr_match(addr, ux->addr))
			continue;
		if (tagged && !rds_tag_match(tag, ignore, ux->hdr.tag))
			continue;
		dlist_remove(&ux->entry);
		return ux;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* delivery						      */
/* ------------------------------------------------------------------ */

static uint64_t rds_rx_flags(struct rds_hdr *hdr, int tagged)
{
	uint64_t flags = FI_RECV | (tagged ? FI_TAGGED : FI_MSG);

	if (hdr->flags & RDS_HF_CQ_DATA)
		flags |= FI_REMOTE_CQ_DATA;
	return flags;
}

static void rds_deliver_eager(struct rds_ep *ep, struct rds_rx_entry *rx,
			      struct rds_hdr *hdr, void *payload, size_t plen,
			      fi_addr_t src)
{
	size_t copied;
	int err = 0;
	int report;

	copied = ofi_copy_to_iov(rx->iov, rx->iov_count, 0, payload, plen);
	if (plen > rx->total_len)
		err = FI_ETRUNC;

	report = (rx->flags | ep->util_ep.rx_op_flags) & FI_COMPLETION;
	if (report || err)
		rds_cq_write_rx(ep, rx->context,
				rds_rx_flags(hdr, !!(hdr->flags & RDS_HF_TAGGED)),
				copied, hdr->data, hdr->tag, src, err);
}

static void rds_rndzv_rx_finalize(struct rds_ep *ep, struct rds_pending *pend)
{
	rds_send_fin(ep, &pend->peer, pend->peer_id);

	if (!pend->no_comp || pend->error)
		rds_cq_write_rx(ep, pend->context, pend->flags, pend->len,
				pend->data, pend->tag, pend->addr, pend->error);

	dlist_remove(&pend->entry);
	free(pend);
}

/*
 * Start a zero-copy rendezvous receive: issue an RDMA READ per source segment
 * straight into the matched application buffer.  @rx may be a transient on-stack
 * descriptor; everything needed past this call is copied into the pending op.
 */
static void rds_start_rndzv_read(struct rds_ep *ep, struct rds_rx_entry *rx,
				 struct rds_hdr *hdr, struct rds_rndzv_seg *segs,
				 size_t seg_cnt, const union ofi_sock_ip *peer,
				 fi_addr_t src)
{
	struct rds_pending *pend;
	size_t effective, off = 0, i;
	int tagged = !!(hdr->flags & RDS_HF_TAGGED);

	pend = rds_pending_alloc(ep, RDS_PEND_RNDZV_RX, rx->context,
				 rds_rx_flags(hdr, tagged));
	if (!pend) {
		/* Drop: nack the sender so it can release its registration. */
		rds_send_fin(ep, peer, hdr->id);
		rds_cq_write_rx(ep, rx->context, rds_rx_flags(hdr, tagged), 0,
				hdr->data, hdr->tag, src, FI_ENOMEM);
		return;
	}

	effective = MIN(hdr->size, rx->total_len);
	pend->len = effective;
	pend->addr = src;
	pend->data = hdr->data;
	pend->tag = hdr->tag;
	pend->peer = *peer;
	pend->peer_id = hdr->id;
	pend->error = (hdr->size > rx->total_len) ? FI_ETRUNC : 0;
	pend->no_comp = !((rx->flags | ep->util_ep.rx_op_flags) & FI_COMPLETION);

	for (i = 0; i < seg_cnt && off < effective; i++) {
		struct iovec liov[RDS_IOV_LIMIT];
		size_t lcnt, this_len;
		ssize_t ret;

		this_len = MIN(segs[i].len, effective - off);
		rds_iov_slice(liov, &lcnt, rx->iov, rx->iov_count, off,
			      this_len);
		off += this_len;

		ret = rds_post_rdma(ep, peer, ofi_sizeofaddr(
					(const struct sockaddr *) peer),
				    (rds_rdma_cookie_t) segs[i].cookie,
				    segs[i].off, liov, lcnt, 0 /* read */,
				    pend->id);
		if (ret) {
			if (!pend->error)
				pend->error = -ret;
			break;
		}
		pend->seg_remaining++;
		pend->seg_total++;
	}

	/* If nothing was issued (zero length or all reads failed) finalize now;
	 * otherwise the last RDS_CMSG_RDMA_STATUS will finalize. */
	if (pend->seg_remaining == 0)
		rds_rndzv_rx_finalize(ep, pend);
}

/* ------------------------------------------------------------------ */
/* inbound dispatch (called from progress)			      */
/* ------------------------------------------------------------------ */

static void rds_enqueue_unexp(struct rds_ep *ep, struct rds_hdr *hdr,
			      void *payload, size_t plen,
			      const union ofi_sock_ip *src, fi_addr_t src_fi,
			      int tagged)
{
	struct rds_unexp *ux;
	size_t copy = (hdr->op == RDS_OP_EAGER) ? plen : 0;

	ux = calloc(1, sizeof(*ux) + copy);
	if (!ux) {
		FI_WARN(&rds_prov, FI_LOG_EP_DATA,
			"out of memory buffering unexpected message; dropping\n");
		if (hdr->op == RDS_OP_RTS)
			rds_send_fin(ep, src, hdr->id);
		return;
	}

	ux->hdr = *hdr;
	ux->addr = src_fi;
	ux->src = *src;

	if (hdr->op == RDS_OP_EAGER) {
		ux->len = plen;
		memcpy(ux->data, payload, plen);
	} else {	/* RTS: stash the segment array for the deferred read */
		ux->seg_cnt = hdr->seg_cnt;
		ux->segs = malloc(hdr->seg_cnt * sizeof(struct rds_rndzv_seg));
		if (!ux->segs) {
			rds_send_fin(ep, src, hdr->id);
			free(ux);
			return;
		}
		memcpy(ux->segs, payload,
		       hdr->seg_cnt * sizeof(struct rds_rndzv_seg));
	}

	dlist_insert_tail(&ux->entry,
			  tagged ? &ep->rx_unexp_tag : &ep->rx_unexp_msg);
}

void rds_handle_inbound(struct rds_ep *ep, struct rds_hdr *hdr, void *payload,
			size_t payload_len, const union ofi_sock_ip *src,
			fi_addr_t src_fi)
{
	struct rds_rx_entry *rx;
	int tagged;

	if (hdr->magic != RDS_HDR_MAGIC) {
		FI_WARN(&rds_prov, FI_LOG_EP_DATA,
			"dropping datagram with bad magic 0x%x\n", hdr->magic);
		return;
	}

	if (hdr->op == RDS_OP_FIN) {
		struct rds_pending *pend = rds_pending_find(ep, hdr->id);
		if (pend && pend->type == RDS_PEND_RNDZV_TX)
			rds_pending_complete(ep, pend);
		return;
	}

	if (hdr->op == RDS_OP_RTS &&
	    payload_len < (size_t) hdr->seg_cnt * sizeof(struct rds_rndzv_seg)) {
		FI_WARN(&rds_prov, FI_LOG_EP_DATA,
			"truncated RTS descriptor (%zu bytes, %u segs)\n",
			payload_len, hdr->seg_cnt);
		return;
	}

	tagged = !!(hdr->flags & RDS_HF_TAGGED);
	rx = rds_match_posted(ep, hdr, src_fi, tagged);
	if (rx) {
		if (hdr->op == RDS_OP_EAGER)
			rds_deliver_eager(ep, rx, hdr, payload, payload_len,
					  src_fi);
		else
			rds_start_rndzv_read(ep, rx, hdr,
					     (struct rds_rndzv_seg *) payload,
					     hdr->seg_cnt, src, src_fi);
		free(rx);
	} else {
		rds_enqueue_unexp(ep, hdr, payload, payload_len, src, src_fi,
				  tagged);
	}
}

void rds_handle_rdma_notify(struct rds_ep *ep, uint64_t token, int status)
{
	struct rds_pending *pend = rds_pending_find(ep, token);

	if (!pend)
		return;

	if (status && !pend->error)
		pend->error = status < 0 ? -status : status;

	if (!pend->seg_remaining)
		return;		/* defensive: stray/duplicate notification */

	if (--pend->seg_remaining)
		return;		/* more segments of this op still outstanding */

	switch (pend->type) {
	case RDS_PEND_RMA:
		rds_pending_complete(ep, pend);
		break;
	case RDS_PEND_RNDZV_RX:
		rds_rndzv_rx_finalize(ep, pend);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* receive posting						      */
/* ------------------------------------------------------------------ */

ssize_t rds_post_recv(struct rds_ep *ep, const struct iovec *iov,
		      size_t iov_count, fi_addr_t addr, uint64_t tag,
		      uint64_t ignore, void *context, uint64_t flags,
		      int tagged)
{
	struct rds_rx_entry *rx;
	struct rds_unexp *ux;
	size_t i;

	if (iov_count > RDS_IOV_LIMIT)
		return -FI_EINVAL;

	ofi_genlock_lock(&ep->util_ep.lock);

	ux = rds_match_unexp(ep, addr, tag, ignore, tagged);
	if (ux) {
		struct rds_rx_entry tmp = {0};

		for (i = 0; i < iov_count; i++)
			tmp.iov[i] = iov[i];
		tmp.iov_count = iov_count;
		tmp.total_len = ofi_total_iov_len(iov, iov_count);
		tmp.context = context;
		tmp.tag = tag;
		tmp.ignore = ignore;
		tmp.addr = addr;
		tmp.flags = flags | (tagged ? FI_TAGGED : 0);

		if (ux->hdr.op == RDS_OP_EAGER)
			rds_deliver_eager(ep, &tmp, &ux->hdr, ux->data,
					  ux->len, ux->addr);
		else
			rds_start_rndzv_read(ep, &tmp, &ux->hdr, ux->segs,
					     ux->seg_cnt, &ux->src, ux->addr);
		free(ux->segs);
		free(ux);
		ofi_genlock_unlock(&ep->util_ep.lock);
		return 0;
	}

	rx = calloc(1, sizeof(*rx));
	if (!rx) {
		ofi_genlock_unlock(&ep->util_ep.lock);
		return -FI_ENOMEM;
	}

	for (i = 0; i < iov_count; i++)
		rx->iov[i] = iov[i];
	rx->iov_count = iov_count;
	rx->total_len = ofi_total_iov_len(iov, iov_count);
	rx->context = context;
	rx->tag = tag;
	rx->ignore = ignore;
	rx->addr = addr;
	rx->flags = flags | (tagged ? FI_TAGGED : 0);

	dlist_insert_tail(&rx->entry,
			  tagged ? &ep->rx_posted_tag : &ep->rx_posted_msg);
	ofi_genlock_unlock(&ep->util_ep.lock);
	return 0;
}

/* ------------------------------------------------------------------ */
/* send							      */
/* ------------------------------------------------------------------ */

static ssize_t rds_send_eager(struct rds_ep *ep, const struct iovec *iov,
			      size_t iov_count, const void *dest,
			      size_t destlen, uint64_t tag, uint64_t data,
			      void *context, uint64_t flags, int tagged,
			      int gen_comp)
{
	struct rds_hdr hdr;
	struct iovec siov[RDS_IOV_LIMIT + 1];
	struct msghdr msg;
	size_t total, i;
	ssize_t ret;

	total = ofi_total_iov_len(iov, iov_count);

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = RDS_HDR_MAGIC;
	hdr.op = RDS_OP_EAGER;
	hdr.flags = (tagged ? RDS_HF_TAGGED : 0) |
		    ((flags & FI_REMOTE_CQ_DATA) ? RDS_HF_CQ_DATA : 0);
	hdr.tag = tag;
	hdr.data = data;
	hdr.size = total;

	siov[0].iov_base = &hdr;
	siov[0].iov_len = sizeof(hdr);
	for (i = 0; i < iov_count; i++)
		siov[i + 1] = iov[i];

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = (void *) dest;
	msg.msg_namelen = (socklen_t) destlen;
	msg.msg_iov = siov;
	msg.msg_iovlen = iov_count + 1;

	ret = sendmsg(ep->sock, &msg, 0);
	if (ret < 0)
		return (errno == EAGAIN || errno == ENOBUFS) ?
			-FI_EAGAIN : -errno;

	if (gen_comp)
		rds_cq_write_tx(ep, context,
				FI_SEND | (tagged ? FI_TAGGED : FI_MSG),
				total, 0);
	return 0;
}

static ssize_t rds_send_rndzv(struct rds_ep *ep, const struct iovec *iov,
			      size_t iov_count, const void *dest,
			      size_t destlen, uint64_t tag, uint64_t data,
			      void *context, uint64_t flags, int tagged,
			      int gen_comp)
{
	struct rds_pending *pend;
	struct rds_rndzv_seg *segs;
	struct rds_mr **mrs;
	struct rds_hdr hdr;
	struct iovec siov[2];
	struct msghdr msg;
	size_t total, nseg = 0, i, k;
	ssize_t ret;

	total = ofi_total_iov_len(iov, iov_count);

	/* One segment per <= 1 MiB chunk of every source iov (the RDS MR limit
	 * is 256 pages, and MRs must be VA-contiguous so iovs are split). */
	for (i = 0; i < iov_count; i++)
		nseg += (iov[i].iov_len + RDS_KERNEL_SEG_MAX - 1) /
			RDS_KERNEL_SEG_MAX;

	if (nseg * sizeof(*segs) > (size_t) RDS_MAX_EAGER_SIZE) {
		FI_WARN(&rds_prov, FI_LOG_EP_DATA,
			"rendezvous descriptor too large (%zu segments); "
			"scatter list unsupported at this size\n", nseg);
		return -FI_EMSGSIZE;
	}

	mrs = calloc(nseg, sizeof(*mrs));
	segs = calloc(nseg, sizeof(*segs));
	if (!mrs || !segs) {
		free(mrs);
		free(segs);
		return -FI_ENOMEM;
	}

	/* Register each source chunk and describe it. */
	k = 0;
	for (i = 0; i < iov_count; i++) {
		char *base = iov[i].iov_base;
		size_t left = iov[i].iov_len;

		while (left) {
			size_t clen = MIN(left, (size_t) RDS_KERNEL_SEG_MAX);
			struct rds_mr *mr;

			mr = rds_reg_internal(ep, base, clen, FI_REMOTE_READ);
			if (!mr) {
				ret = -FI_ENOMEM;
				goto err_unreg;
			}
			mrs[k] = mr;
			segs[k].cookie = (uint64_t) mr->cookie;
			segs[k].off = 0;
			segs[k].len = clen;
			k++;
			base += clen;
			left -= clen;
		}
	}

	pend = rds_pending_alloc(ep, RDS_PEND_RNDZV_TX, context,
				 FI_SEND | (tagged ? FI_TAGGED : FI_MSG));
	if (!pend) {
		ret = -FI_ENOMEM;
		goto err_unreg;
	}
	pend->len = total;
	pend->mrs = mrs;
	pend->mr_cnt = nseg;
	pend->no_comp = !gen_comp;

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = RDS_HDR_MAGIC;
	hdr.op = RDS_OP_RTS;
	hdr.flags = (tagged ? RDS_HF_TAGGED : 0) |
		    ((flags & FI_REMOTE_CQ_DATA) ? RDS_HF_CQ_DATA : 0);
	hdr.seg_cnt = (uint16_t) nseg;
	hdr.tag = tag;
	hdr.data = data;
	hdr.size = total;
	hdr.id = pend->id;

	siov[0].iov_base = &hdr;
	siov[0].iov_len = sizeof(hdr);
	siov[1].iov_base = segs;
	siov[1].iov_len = nseg * sizeof(*segs);

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = (void *) dest;
	msg.msg_namelen = (socklen_t) destlen;
	msg.msg_iov = siov;
	msg.msg_iovlen = 2;

	ret = sendmsg(ep->sock, &msg, 0);
	if (ret < 0) {
		ret = (errno == EAGAIN || errno == ENOBUFS) ?
			-FI_EAGAIN : -errno;
		dlist_remove(&pend->entry);
		free(pend);
		goto err_unreg;
	}

	free(segs);	/* the kernel copied them during sendmsg */
	/* mrs ownership transferred to pend; freed on FIN. */
	return 0;

err_unreg:
	for (i = 0; i < k; i++)
		rds_mr_put(mrs[i]);
	free(mrs);
	free(segs);
	return ret;
}

ssize_t rds_generic_send(struct rds_ep *ep, const struct iovec *iov,
			 size_t iov_count, fi_addr_t dest_addr, uint64_t tag,
			 uint64_t data, void *context, uint64_t flags,
			 int tagged)
{
	const void *dest;
	size_t total, destlen;
	int gen_comp;
	ssize_t ret;

	if (iov_count > RDS_IOV_LIMIT)
		return -FI_EINVAL;

	total = ofi_total_iov_len(iov, iov_count);
	if (total > RDS_MAX_MSG_SIZE)
		return -FI_EMSGSIZE;

	dest = rds_av_addr(ep, dest_addr);
	if (!dest)
		return -FI_EINVAL;
	destlen = ep->util_ep.av->addrlen;

	gen_comp = !!((ep->util_ep.tx_op_flags | flags) & FI_COMPLETION);

	ofi_genlock_lock(&ep->util_ep.lock);
	if (total <= rds_eager_size)
		ret = rds_send_eager(ep, iov, iov_count, dest, destlen, tag,
				     data, context, flags, tagged, gen_comp);
	else
		ret = rds_send_rndzv(ep, iov, iov_count, dest, destlen, tag,
				     data, context, flags, tagged, gen_comp);
	ofi_genlock_unlock(&ep->util_ep.lock);
	return ret;
}

/* inject: small eager send, no completion, buffer reusable on return. */
static ssize_t rds_generic_inject(struct rds_ep *ep, const void *buf,
				  size_t len, fi_addr_t dest_addr, uint64_t tag,
				  int tagged)
{
	struct iovec iov;
	const void *dest;
	ssize_t ret;

	if (len > rds_eager_size)
		return -FI_EMSGSIZE;

	dest = rds_av_addr(ep, dest_addr);
	if (!dest)
		return -FI_EINVAL;

	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	ofi_genlock_lock(&ep->util_ep.lock);
	ret = rds_send_eager(ep, &iov, 1, dest, ep->util_ep.av->addrlen, tag,
			     0, NULL, 0, tagged, 0 /* no completion */);
	ofi_genlock_unlock(&ep->util_ep.lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* fi_ops_msg						      */
/* ------------------------------------------------------------------ */

static ssize_t rds_recv(struct fid_ep *ep_fid, void *buf, size_t len,
			void *desc, fi_addr_t src_addr, void *context)
{
	struct rds_ep *ep = container_of(ep_fid, struct rds_ep,
					 util_ep.ep_fid.fid);
	struct iovec iov = { buf, len };

	return rds_post_recv(ep, &iov, 1, src_addr, 0, 0, context, 0, 0);
}

static ssize_t rds_recvv(struct fid_ep *ep_fid, const struct iovec *iov,
			 void **desc, size_t count, fi_addr_t src_addr,
			 void *context)
{
	struct rds_ep *ep = container_of(ep_fid, struct rds_ep,
					 util_ep.ep_fid.fid);

	return rds_post_recv(ep, iov, count, src_addr, 0, 0, context, 0, 0);
}

static ssize_t rds_recvmsg(struct fid_ep *ep_fid, const struct fi_msg *msg,
			   uint64_t flags)
{
	struct rds_ep *ep = container_of(ep_fid, struct rds_ep,
					 util_ep.ep_fid.fid);

	return rds_post_recv(ep, msg->msg_iov, msg->iov_count, msg->addr, 0, 0,
			     msg->context, flags, 0);
}

static ssize_t rds_send(struct fid_ep *ep_fid, const void *buf, size_t len,
			void *desc, fi_addr_t dest_addr, void *context)
{
	struct rds_ep *ep = container_of(ep_fid, struct rds_ep,
					 util_ep.ep_fid.fid);
	struct iovec iov = { (void *) buf, len };

	return rds_generic_send(ep, &iov, 1, dest_addr, 0, 0, context, 0, 0);
}

static ssize_t rds_sendv(struct fid_ep *ep_fid, const struct iovec *iov,
			 void **desc, size_t count, fi_addr_t dest_addr,
			 void *context)
{
	struct rds_ep *ep = container_of(ep_fid, struct rds_ep,
					 util_ep.ep_fid.fid);

	return rds_generic_send(ep, iov, count, dest_addr, 0, 0, context, 0, 0);
}

static ssize_t rds_sendmsg(struct fid_ep *ep_fid, const struct fi_msg *msg,
			   uint64_t flags)
{
	struct rds_ep *ep = container_of(ep_fid, struct rds_ep,
					 util_ep.ep_fid.fid);

	return rds_generic_send(ep, msg->msg_iov, msg->iov_count, msg->addr, 0,
				msg->data, msg->context, flags, 0);
}

static ssize_t rds_inject(struct fid_ep *ep_fid, const void *buf, size_t len,
			  fi_addr_t dest_addr)
{
	struct rds_ep *ep = container_of(ep_fid, struct rds_ep,
					 util_ep.ep_fid.fid);

	return rds_generic_inject(ep, buf, len, dest_addr, 0, 0);
}

static ssize_t rds_senddata(struct fid_ep *ep_fid, const void *buf, size_t len,
			    void *desc, uint64_t data, fi_addr_t dest_addr,
			    void *context)
{
	struct rds_ep *ep = container_of(ep_fid, struct rds_ep,
					 util_ep.ep_fid.fid);
	struct iovec iov = { (void *) buf, len };

	return rds_generic_send(ep, &iov, 1, dest_addr, 0, data, context,
				FI_REMOTE_CQ_DATA, 0);
}

struct fi_ops_msg rds_msg_ops = {
	.size		= sizeof(struct fi_ops_msg),
	.recv		= rds_recv,
	.recvv		= rds_recvv,
	.recvmsg	= rds_recvmsg,
	.send		= rds_send,
	.sendv		= rds_sendv,
	.sendmsg	= rds_sendmsg,
	.inject		= rds_inject,
	.senddata	= rds_senddata,
	.injectdata	= fi_no_msg_injectdata,
};

/* ------------------------------------------------------------------ */
/* fi_ops_tagged						      */
/* ------------------------------------------------------------------ */

static ssize_t rds_trecv(struct fid_ep *ep_fid, void *buf, size_t len,
			 void *desc, fi_addr_t src_addr, uint64_t tag,
			 uint64_t ignore, void *context)
{
	struct rds_ep *ep = container_of(ep_fid, struct rds_ep,
					 util_ep.ep_fid.fid);
	struct iovec iov = { buf, len };

	return rds_post_recv(ep, &iov, 1, src_addr, tag, ignore, context, 0, 1);
}

static ssize_t rds_trecvv(struct fid_ep *ep_fid, const struct iovec *iov,
			  void **desc, size_t count, fi_addr_t src_addr,
			  uint64_t tag, uint64_t ignore, void *context)
{
	struct rds_ep *ep = container_of(ep_fid, struct rds_ep,
					 util_ep.ep_fid.fid);

	return rds_post_recv(ep, iov, count, src_addr, tag, ignore, context, 0,
			     1);
}

static ssize_t rds_trecvmsg(struct fid_ep *ep_fid,
			    const struct fi_msg_tagged *msg, uint64_t flags)
{
	struct rds_ep *ep = container_of(ep_fid, struct rds_ep,
					 util_ep.ep_fid.fid);

	return rds_post_recv(ep, msg->msg_iov, msg->iov_count, msg->addr,
			     msg->tag, msg->ignore, msg->context, flags, 1);
}

static ssize_t rds_tsend(struct fid_ep *ep_fid, const void *buf, size_t len,
			 void *desc, fi_addr_t dest_addr, uint64_t tag,
			 void *context)
{
	struct rds_ep *ep = container_of(ep_fid, struct rds_ep,
					 util_ep.ep_fid.fid);
	struct iovec iov = { (void *) buf, len };

	return rds_generic_send(ep, &iov, 1, dest_addr, tag, 0, context, 0, 1);
}

static ssize_t rds_tsendv(struct fid_ep *ep_fid, const struct iovec *iov,
			  void **desc, size_t count, fi_addr_t dest_addr,
			  uint64_t tag, void *context)
{
	struct rds_ep *ep = container_of(ep_fid, struct rds_ep,
					 util_ep.ep_fid.fid);

	return rds_generic_send(ep, iov, count, dest_addr, tag, 0, context, 0,
				1);
}

static ssize_t rds_tsendmsg(struct fid_ep *ep_fid,
			    const struct fi_msg_tagged *msg, uint64_t flags)
{
	struct rds_ep *ep = container_of(ep_fid, struct rds_ep,
					 util_ep.ep_fid.fid);

	return rds_generic_send(ep, msg->msg_iov, msg->iov_count, msg->addr,
				msg->tag, msg->data, msg->context, flags, 1);
}

static ssize_t rds_tinject(struct fid_ep *ep_fid, const void *buf, size_t len,
			   fi_addr_t dest_addr, uint64_t tag)
{
	struct rds_ep *ep = container_of(ep_fid, struct rds_ep,
					 util_ep.ep_fid.fid);

	return rds_generic_inject(ep, buf, len, dest_addr, tag, 1);
}

static ssize_t rds_tsenddata(struct fid_ep *ep_fid, const void *buf,
			     size_t len, void *desc, uint64_t data,
			     fi_addr_t dest_addr, uint64_t tag, void *context)
{
	struct rds_ep *ep = container_of(ep_fid, struct rds_ep,
					 util_ep.ep_fid.fid);
	struct iovec iov = { (void *) buf, len };

	return rds_generic_send(ep, &iov, 1, dest_addr, tag, data, context,
				FI_REMOTE_CQ_DATA, 1);
}

struct fi_ops_tagged rds_tagged_ops = {
	.size		= sizeof(struct fi_ops_tagged),
	.recv		= rds_trecv,
	.recvv		= rds_trecvv,
	.recvmsg	= rds_trecvmsg,
	.send		= rds_tsend,
	.sendv		= rds_tsendv,
	.sendmsg	= rds_tsendmsg,
	.inject		= rds_tinject,
	.senddata	= rds_tsenddata,
	.injectdata	= fi_no_tagged_injectdata,
};

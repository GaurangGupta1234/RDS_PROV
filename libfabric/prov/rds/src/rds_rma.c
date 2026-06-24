/*
 * libfabric RDS provider - one-sided RMA (the pure zero-copy path).
 *
 * fi_write / fi_read map directly onto an RDS RDMA op: a sendmsg that carries
 * only an RDS_CMSG_RDMA_ARGS control message (no payload).  The kernel pins the
 * local pages, the HCA DMAs straight to/from the peer's registered buffer
 * (identified by the cookie == libfabric key), and a completion notification is
 * delivered back to us as RDS_CMSG_RDMA_STATUS, reaped in rds_progress.c.
 *
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 */

#include <stdlib.h>
#include "rds.h"

ssize_t rds_post_rdma(struct rds_ep *ep, const void *dest_addr,
		      size_t dest_addrlen, rds_rdma_cookie_t cookie,
		      uint64_t remote_off, const struct iovec *local_iov,
		      size_t iov_cnt, int is_write, uint64_t token)
{
	struct msghdr msg;
	struct cmsghdr *cmsg;
	struct rds_rdma_args args;
	struct rds_iovec liov[RDS_IOV_LIMIT];
	char cbuf[CMSG_SPACE(sizeof(struct rds_rdma_args))];
	size_t i, total = 0;
	ssize_t ret;

	if (iov_cnt == 0 || iov_cnt > RDS_IOV_LIMIT)
		return -FI_EINVAL;

	for (i = 0; i < iov_cnt; i++) {
		liov[i].addr = (uint64_t) (uintptr_t) local_iov[i].iov_base;
		liov[i].bytes = local_iov[i].iov_len;
		total += local_iov[i].iov_len;
	}

	memset(&args, 0, sizeof(args));
	args.cookie = cookie;
	args.remote_vec.addr = remote_off;	/* offset into the remote MR  */
	args.remote_vec.bytes = total;
	args.local_vec_addr = (uint64_t) (uintptr_t) liov;
	args.nr_local = iov_cnt;
	args.flags = RDS_RDMA_NOTIFY_ME | (is_write ? RDS_RDMA_READWRITE : 0);
	args.user_token = token;

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = (void *) dest_addr;
	msg.msg_namelen = (socklen_t) dest_addrlen;
	msg.msg_control = cbuf;
	msg.msg_controllen = CMSG_SPACE(sizeof(args));

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_RDS;
	cmsg->cmsg_type = RDS_CMSG_RDMA_ARGS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(args));
	memcpy(CMSG_DATA(cmsg), &args, sizeof(args));
	msg.msg_controllen = cmsg->cmsg_len;

	/* sendmsg copies the args + the rds_iovec array into the kernel during
	 * the call, so the stack-local liov is safe to drop afterwards. */
	ret = sendmsg(ep->sock, &msg, 0);
	if (ret < 0)
		return (errno == EAGAIN || errno == ENOBUFS) ?
			-FI_EAGAIN : -errno;
	return 0;
}

/*
 * Common path for fi_read/fi_write and their v/msg variants.
 *
 * A single RDS RDMA op (and MR) is capped at 1 MiB, so larger transfers are
 * split into <=1 MiB chunks issued under one pending op (one libfabric
 * completion).  Each chunk reads/writes a contiguous offset window of the
 * remote MR and the corresponding slice of the local iov -- still zero copy.
 */
static ssize_t rds_rma_common(struct rds_ep *ep, const struct iovec *iov,
			      size_t iov_count, fi_addr_t remote_fi,
			      uint64_t remote_off, uint64_t key, int is_write,
			      void *context, uint64_t flags)
{
	struct rds_pending *pend;
	const void *dest;
	size_t total, off = 0, addrlen;
	ssize_t ret = 0;

	total = rds_iov_len(iov, iov_count);
	if (total > RDS_MAX_MSG_SIZE)
		return -FI_EMSGSIZE;

	dest = rds_av_addr(ep, remote_fi);
	if (!dest)
		return -FI_EINVAL;
	addrlen = ep->util_ep.av->addrlen;

	ofi_genlock_lock(&ep->util_ep.lock);

	pend = rds_pending_alloc(ep, RDS_PEND_RMA, context,
				 FI_RMA | (is_write ? FI_WRITE : FI_READ));
	if (!pend) {
		ret = -FI_ENOMEM;
		goto out;
	}
	pend->len = total;
	pend->addr = remote_fi;
	pend->no_comp = !((ep->util_ep.tx_op_flags | flags) & FI_COMPLETION);

	while (off < total) {
		struct iovec liov[RDS_IOV_LIMIT];
		size_t lcnt, got;
		size_t clen = MIN(total - off, (size_t) RDS_KERNEL_SEG_MAX);

		/* A chunk may cover fewer bytes than clen if the local iov is
		 * highly fragmented (>RDS_IOV_LIMIT segments in the window); we
		 * advance by what was actually mapped so offsets stay aligned. */
		got = rds_iov_slice(liov, &lcnt, iov, iov_count, off, clen);
		if (got == 0) {
			ret = -FI_EINVAL;
			break;
		}
		ret = rds_post_rdma(ep, dest, addrlen, (rds_rdma_cookie_t) key,
				    remote_off + off, liov, lcnt, is_write,
				    pend->id);
		if (ret)
			break;
		pend->seg_remaining++;
		off += got;
	}

	if (pend->seg_remaining == 0) {
		/* nothing issued (e.g. first chunk failed) -> no notify coming */
		dlist_remove(&pend->entry);
		free(pend);
	} else {
		/* at least one chunk in flight; its notification(s) complete the
		 * op.  A mid-stream failure is recorded and surfaced then. */
		if (ret)
			pend->error = -ret;
		ret = 0;
	}
out:
	ofi_genlock_unlock(&ep->util_ep.lock);
	return ret;
}

static ssize_t rds_write(struct fid_ep *ep_fid, const void *buf, size_t len,
			 void *desc, fi_addr_t dest_addr, uint64_t addr,
			 uint64_t key, void *context)
{
	struct rds_ep *ep;
	struct iovec iov;

	ep = container_of(ep_fid, struct rds_ep, util_ep.ep_fid.fid);
	iov.iov_base = (void *) buf;
	iov.iov_len = len;
	return rds_rma_common(ep, &iov, 1, dest_addr, addr, key, 1, context, 0);
}

static ssize_t rds_writev(struct fid_ep *ep_fid, const struct iovec *iov,
			  void **desc, size_t count, fi_addr_t dest_addr,
			  uint64_t addr, uint64_t key, void *context)
{
	struct rds_ep *ep;

	ep = container_of(ep_fid, struct rds_ep, util_ep.ep_fid.fid);
	return rds_rma_common(ep, iov, count, dest_addr, addr, key, 1, context,
			      0);
}

static ssize_t rds_writemsg(struct fid_ep *ep_fid, const struct fi_msg_rma *msg,
			    uint64_t flags)
{
	struct rds_ep *ep;

	if (msg->rma_iov_count != 1)
		return -FI_EINVAL;

	ep = container_of(ep_fid, struct rds_ep, util_ep.ep_fid.fid);
	return rds_rma_common(ep, msg->msg_iov, msg->iov_count, msg->addr,
			      msg->rma_iov[0].addr, msg->rma_iov[0].key, 1,
			      msg->context, flags);
}

static ssize_t rds_read(struct fid_ep *ep_fid, void *buf, size_t len,
			void *desc, fi_addr_t src_addr, uint64_t addr,
			uint64_t key, void *context)
{
	struct rds_ep *ep;
	struct iovec iov;

	ep = container_of(ep_fid, struct rds_ep, util_ep.ep_fid.fid);
	iov.iov_base = buf;
	iov.iov_len = len;
	return rds_rma_common(ep, &iov, 1, src_addr, addr, key, 0, context, 0);
}

static ssize_t rds_readv(struct fid_ep *ep_fid, const struct iovec *iov,
			 void **desc, size_t count, fi_addr_t src_addr,
			 uint64_t addr, uint64_t key, void *context)
{
	struct rds_ep *ep;

	ep = container_of(ep_fid, struct rds_ep, util_ep.ep_fid.fid);
	return rds_rma_common(ep, iov, count, src_addr, addr, key, 0, context,
			      0);
}

static ssize_t rds_readmsg(struct fid_ep *ep_fid, const struct fi_msg_rma *msg,
			   uint64_t flags)
{
	struct rds_ep *ep;

	if (msg->rma_iov_count != 1)
		return -FI_EINVAL;

	ep = container_of(ep_fid, struct rds_ep, util_ep.ep_fid.fid);
	return rds_rma_common(ep, msg->msg_iov, msg->iov_count, msg->addr,
			      msg->rma_iov[0].addr, msg->rma_iov[0].key, 0,
			      msg->context, flags);
}

static ssize_t rds_inject_write(struct fid_ep *ep_fid, const void *buf,
				size_t len, fi_addr_t dest_addr, uint64_t addr,
				uint64_t key)
{
	struct rds_ep *ep;
	struct rds_pending *pend;
	struct iovec iov;
	const void *dest;
	void *bounce;
	ssize_t ret;

	ep = container_of(ep_fid, struct rds_ep, util_ep.ep_fid.fid);
	if (len > rds_eager_size)
		return -FI_EMSGSIZE;

	/*
	 * fi_inject requires the source buffer be reusable on return, but an
	 * RDS RDMA op may still be reading it after sendmsg returns.  Copy into
	 * a private bounce that we free when the op's notification arrives.  No
	 * completion is generated (inject semantics).
	 */
	bounce = malloc(len);
	if (!bounce)
		return -FI_ENOMEM;
	memcpy(bounce, buf, len);

	dest = rds_av_addr(ep, dest_addr);
	if (!dest) {
		free(bounce);
		return -FI_EINVAL;
	}

	ofi_genlock_lock(&ep->util_ep.lock);
	pend = rds_pending_alloc(ep, RDS_PEND_RMA, NULL, FI_RMA | FI_WRITE);
	if (!pend) {
		ret = -FI_ENOMEM;
		goto err;
	}
	pend->no_comp = 1;
	pend->bounce = bounce;
	pend->seg_remaining = 1;	/* completed by the single RDMA notify */

	iov.iov_base = bounce;
	iov.iov_len = len;
	ret = rds_post_rdma(ep, dest, ep->util_ep.av->addrlen,
			    (rds_rdma_cookie_t) key, addr, &iov, 1, 1,
			    pend->id);
	if (ret) {
		dlist_remove(&pend->entry);
		free(pend);
		goto err;
	}
	ofi_genlock_unlock(&ep->util_ep.lock);
	return 0;
err:
	ofi_genlock_unlock(&ep->util_ep.lock);
	free(bounce);
	return ret;
}

struct fi_ops_rma rds_rma_ops = {
	.size		= sizeof(struct fi_ops_rma),
	.read		= rds_read,
	.readv		= rds_readv,
	.readmsg	= rds_readmsg,
	.write		= rds_write,
	.writev		= rds_writev,
	.writemsg	= rds_writemsg,
	.inject		= rds_inject_write,
	.writedata	= fi_no_rma_writedata,
	.injectdata	= fi_no_rma_injectdata,
};

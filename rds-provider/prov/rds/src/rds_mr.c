/*
 * libfabric RDS provider - memory registration (the zero-copy enabler).
 *
 * fi_mr_reg() -> setsockopt(RDS_GET_MR): the kernel pins the user pages and
 * hands back an rds_rdma_cookie_t.  We export that cookie *verbatim* as the
 * libfabric mr_key.  A peer that learns the key drops it straight into
 * rds_rdma_args.cookie to read/write these pages over the wire with no bounce
 * copy.  Because the cookie's r_key is validated by the target HCA, the
 * provider keeps no key map of its own.
 *
 * Stock-kernel limits (see docs/RDS_KERNEL_CHANGES.md): a single MR is capped
 * at 256 pages (1 MiB).  Registrations larger than that fail here; the
 * rendezvous path in rds_msg.c works around the limit by registering source
 * buffers one <=1 MiB segment at a time.
 *
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 */

#include <stdlib.h>
#include "rds.h"

/*
 * Low level: pin @buf/@len on @sock and return the cookie.  Used both by
 * fi_mr_reg (app windows) and by the internal rendezvous source registration.
 */
static int rds_kernel_reg(int sock, const void *buf, size_t len,
			  uint64_t access, rds_rdma_cookie_t *cookie_out)
{
	struct rds_get_mr_args args;
	rds_rdma_cookie_t cookie = 0;
	uint64_t flags = 0;

	if (len > RDS_KERNEL_SEG_MAX) {
		FI_WARN(&rds_prov, FI_LOG_MR,
			"MR of %zu bytes exceeds the RDS per-MR limit of %d "
			"bytes (256 pages). Patch the kernel module or split "
			"the region.\n", len, RDS_KERNEL_SEG_MAX);
		return -FI_EINVAL;
	}

	/*
	 * RDS_RDMA_READWRITE on registration means "allow remote writes" (read
	 * is always permitted).  MPI windows need both, and our rendezvous
	 * source buffers are read remotely, so request it unconditionally.
	 */
	if (access & (FI_REMOTE_WRITE | FI_REMOTE_READ | FI_WRITE | FI_READ))
		flags |= RDS_RDMA_READWRITE;

	memset(&args, 0, sizeof(args));
	args.vec.addr = (uint64_t) (uintptr_t) buf;
	args.vec.bytes = len;
	args.cookie_addr = (uint64_t) (uintptr_t) &cookie;
	args.flags = flags;

	if (setsockopt(sock, SOL_RDS, RDS_GET_MR, &args, sizeof(args))) {
		FI_WARN(&rds_prov, FI_LOG_MR,
			"RDS_GET_MR failed: %s. If this is ENOTCONN/EINVAL the "
			"registration socket may need a proxy QP (see "
			"RDS_KERNEL_CHANGES.md).\n", strerror(errno));
		return -errno;
	}

	*cookie_out = cookie;
	return 0;
}

static void rds_kernel_free(int sock, rds_rdma_cookie_t cookie)
{
	struct rds_free_mr_args args;

	memset(&args, 0, sizeof(args));
	args.cookie = cookie;
	args.flags = 0;
	(void) setsockopt(sock, SOL_RDS, RDS_FREE_MR, &args, sizeof(args));
}

/*
 * Internal registration used by the rendezvous send path.  Refcounted so a
 * future MR cache can hand the same registration to several in-flight sends.
 */
struct rds_mr *rds_reg_internal(struct rds_ep *ep, const void *buf, size_t len,
				uint64_t access)
{
	struct rds_mr *mr;
	rds_rdma_cookie_t cookie;

	if (rds_kernel_reg(ep->sock, buf, len, access, &cookie))
		return NULL;

	mr = calloc(1, sizeof(*mr));
	if (!mr) {
		rds_kernel_free(ep->sock, cookie);
		return NULL;
	}

	mr->ep = ep;
	mr->domain = container_of(ep->util_ep.domain, struct rds_domain,
				  util_domain);
	mr->cookie = cookie;
	mr->offset = rds_cookie_off(cookie);
	mr->buf = (void *) buf;
	mr->len = len;
	mr->access = access;
	ofi_atomic_initialize32(&mr->ref, 1);
	return mr;
}

void rds_mr_put(struct rds_mr *mr)
{
	if (!mr || ofi_atomic_dec32(&mr->ref))
		return;
	if (mr->ep)
		rds_kernel_free(mr->ep->sock, mr->cookie);
	free(mr);
}

static int rds_mr_close(struct fid *fid)
{
	struct rds_mr *mr;

	mr = container_of(fid, struct rds_mr, mr_fid.fid);
	rds_mr_put(mr);
	return 0;
}

static struct fi_ops rds_mr_fi_ops = {
	.size		= sizeof(struct fi_ops),
	.close		= rds_mr_close,
	.bind		= fi_no_bind,
	.control	= fi_no_control,
	.ops_open	= fi_no_ops_open,
};

static int rds_mr_regattr(struct fid *fid, const struct fi_mr_attr *attr,
			  uint64_t flags, struct fid_mr **mr_fid)
{
	struct rds_domain *domain;
	struct rds_mr *mr;
	const void *buf;
	size_t len;

	if (!attr || attr->iov_count != 1) {
		FI_WARN(&rds_prov, FI_LOG_MR,
			"RDS MRs must describe exactly one contiguous region\n");
		return -FI_EINVAL;
	}

	domain = container_of(fid, struct rds_domain,
			      util_domain.domain_fid.fid);
	if (!domain->reg_ep) {
		FI_WARN(&rds_prov, FI_LOG_MR,
			"fi_mr_reg before an endpoint is enabled: RDS needs a "
			"bound socket to register against a device. Enable the "
			"endpoint first.\n");
		return -FI_EOPBADSTATE;
	}

	buf = attr->mr_iov[0].iov_base;
	len = attr->mr_iov[0].iov_len;

	mr = rds_reg_internal(domain->reg_ep, buf, len, attr->access);
	if (!mr)
		return -FI_ENOMEM;

	mr->mr_fid.fid.fclass = FI_CLASS_MR;
	mr->mr_fid.fid.context = attr->context;
	mr->mr_fid.fid.ops = &rds_mr_fi_ops;
	/* mem_desc is handed back as the local "desc"; we never dereference it
	 * on the bulk path (the kernel pins local buffers on demand). */
	mr->mr_fid.mem_desc = mr;
	mr->mr_fid.key = (uint64_t) mr->cookie;	/* the cookie IS the key */

	*mr_fid = &mr->mr_fid;
	return 0;
}

static int rds_mr_regv(struct fid *fid, const struct iovec *iov, size_t count,
		       uint64_t access, uint64_t offset, uint64_t requested_key,
		       uint64_t flags, struct fid_mr **mr_fid, void *context)
{
	struct fi_mr_attr attr = {0};

	attr.mr_iov = iov;
	attr.iov_count = count;
	attr.access = access;
	attr.offset = offset;
	attr.requested_key = requested_key;
	attr.context = context;
	return rds_mr_regattr(fid, &attr, flags, mr_fid);
}

static int rds_mr_reg(struct fid *fid, const void *buf, size_t len,
		      uint64_t access, uint64_t offset, uint64_t requested_key,
		      uint64_t flags, struct fid_mr **mr_fid, void *context)
{
	struct iovec iov;

	iov.iov_base = (void *) buf;
	iov.iov_len = len;
	return rds_mr_regv(fid, &iov, 1, access, offset, requested_key, flags,
			   mr_fid, context);
}

struct fi_ops_mr rds_mr_ops = {
	.size		= sizeof(struct fi_ops_mr),
	.reg		= rds_mr_reg,
	.regv		= rds_mr_regv,
	.regattr	= rds_mr_regattr,
};

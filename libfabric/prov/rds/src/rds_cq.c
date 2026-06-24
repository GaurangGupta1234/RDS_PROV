/*
 * libfabric RDS provider - completion queue.
 *
 * Thin wrapper over util_cq.  Progress is driven from ofi_cq_progress(), which
 * walks the CQ's bound endpoints and calls rds_ep_progress() on each -- that is
 * where the RDS socket is drained and completions are generated.
 *
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 */

#include <stdlib.h>
#include "rds.h"

static int rds_cq_close(struct fid *fid)
{
	struct util_cq *cq;
	int ret;

	cq = container_of(fid, struct util_cq, cq_fid.fid);
	ret = ofi_cq_cleanup(cq);
	if (ret)
		return ret;
	free(cq);
	return 0;
}

static struct fi_ops rds_cq_fi_ops = {
	.size		= sizeof(struct fi_ops),
	.close		= rds_cq_close,
	.bind		= fi_no_bind,
	.control	= ofi_cq_control,
	.ops_open	= fi_no_ops_open,
};

int rds_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
		struct fid_cq **cq_fid, void *context)
{
	struct util_cq *cq;
	int ret;

	cq = calloc(1, sizeof(*cq));
	if (!cq)
		return -FI_ENOMEM;

	ret = ofi_cq_init(&rds_prov, domain, attr, cq, &ofi_cq_progress,
			  context);
	if (ret) {
		free(cq);
		return ret;
	}

	*cq_fid = &cq->cq_fid;
	(*cq_fid)->fid.ops = &rds_cq_fi_ops;
	return 0;
}

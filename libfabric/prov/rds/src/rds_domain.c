/*
 * libfabric RDS provider - domain object.
 *
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 */

#include <stdlib.h>
#include "rds.h"

static struct fi_ops_domain rds_domain_ops = {
	.size			= sizeof(struct fi_ops_domain),
	.av_open		= ofi_ip_av_create,
	.cq_open		= rds_cq_open,
	.endpoint		= rds_endpoint,
	.scalable_ep		= fi_no_scalable_ep,
	.cntr_open		= fi_no_cntr_open,
	.poll_open		= fi_poll_create,
	.stx_ctx		= fi_no_stx_context,
	.srx_ctx		= fi_no_srx_context,
	.query_atomic		= fi_no_query_atomic,
	.query_collective	= fi_no_query_collective,
};

static int rds_domain_close(fid_t fid)
{
	struct rds_domain *domain;
	int ret;

	domain = container_of(fid, struct rds_domain,
			      util_domain.domain_fid.fid);
	ret = ofi_domain_close(&domain->util_domain);
	if (ret)
		return ret;
	free(domain);
	return 0;
}

static struct fi_ops rds_domain_fi_ops = {
	.size		= sizeof(struct fi_ops),
	.close		= rds_domain_close,
	.bind		= fi_no_bind,
	.control	= fi_no_control,
	.ops_open	= fi_no_ops_open,
};

int rds_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		    struct fid_domain **domain, void *context)
{
	struct rds_domain *rds_domain;
	int ret;

	ret = ofi_prov_check_info(&rds_util_prov, fabric->api_version, info);
	if (ret)
		return ret;

	rds_domain = calloc(1, sizeof(*rds_domain));
	if (!rds_domain)
		return -FI_ENOMEM;

	ret = ofi_domain_init(fabric, info, &rds_domain->util_domain, context,
			      OFI_LOCK_MUTEX);
	if (ret) {
		free(rds_domain);
		return ret;
	}

	*domain = &rds_domain->util_domain.domain_fid;
	(*domain)->fid.ops = &rds_domain_fi_ops;
	(*domain)->ops = &rds_domain_ops;
	(*domain)->mr = &rds_mr_ops;
	return 0;
}

/*
 * libfabric RDS provider - domain object.
 *
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 */

#include <stdlib.h>
#include "rds.h"

/*
 * Completion counters (FI_CNTR).  Intel MPI's CH4/OFI init binds a counter to
 * the endpoint and aborts MPI_Init with "unsupported feature" if the provider
 * stubs cntr_open out, so we must provide one.  ofi_cntr_progress drives
 * rds_ep_progress when the counter is read, and ofi_cntr_init asserts a non-NULL
 * progress fn (the golden patch's NULL aborts debug builds -- fixed here).
 *
 * Note: the counter is advertised but its value is not yet incremented on
 * completion (the message path uses the CQ, and MPI runs with RMA/atomics
 * disabled, so nothing waits on the counter).  Wiring ofi_cntr increments into
 * rds_cq_write_tx/rx is future work -- see docs/ARCHITECTURE.md.
 */
static int rds_cntr_open(struct fid_domain *domain, struct fi_cntr_attr *attr,
			 struct fid_cntr **cntr_fid, void *context)
{
	struct util_cntr *cntr;
	int ret;

	cntr = calloc(1, sizeof(*cntr));
	if (!cntr)
		return -FI_ENOMEM;

	ret = ofi_cntr_init(&rds_prov, domain, attr, cntr, &ofi_cntr_progress,
			    context);
	if (ret) {
		free(cntr);
		return ret;
	}

	*cntr_fid = &cntr->cntr_fid;
	return 0;
}

static struct fi_ops_domain rds_domain_ops = {
	.size			= sizeof(struct fi_ops_domain),
	.av_open		= ofi_ip_av_create,
	.cq_open		= rds_cq_open,
	.endpoint		= rds_endpoint,
	.scalable_ep		= fi_no_scalable_ep,
	.cntr_open		= rds_cntr_open,
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
	rds_mr_cache_close(domain);
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

	/* Best-effort: if the cache can't init we fall back to per-message
	 * registration, so ignore the return code here. */
	rds_mr_cache_open(rds_domain);

	*domain = &rds_domain->util_domain.domain_fid;
	(*domain)->fid.ops = &rds_domain_fi_ops;
	(*domain)->ops = &rds_domain_ops;
	(*domain)->mr = &rds_mr_ops;
	return 0;
}

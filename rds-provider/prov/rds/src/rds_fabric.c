/*
 * libfabric RDS provider - fabric object.
 *
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 */

#include <stdlib.h>
#include "rds.h"

static struct fi_ops_fabric rds_fabric_ops = {
	.size		= sizeof(struct fi_ops_fabric),
	.domain		= rds_domain_open,
	.passive_ep	= fi_no_passive_ep,
	.eq_open	= ofi_eq_create,
	.wait_open	= ofi_wait_fd_open,
	.trywait	= ofi_trywait,
};

static int rds_fabric_close(fid_t fid)
{
	struct util_fabric *fabric;
	int ret;

	fabric = container_of(fid, struct util_fabric, fabric_fid.fid);
	ret = ofi_fabric_close(fabric);
	if (ret)
		return ret;
	free(fabric);
	return 0;
}

static struct fi_ops rds_fabric_fi_ops = {
	.size		= sizeof(struct fi_ops),
	.close		= rds_fabric_close,
	.bind		= fi_no_bind,
	.control	= fi_no_control,
	.ops_open	= fi_no_ops_open,
};

int rds_fabric(struct fi_fabric_attr *attr, struct fid_fabric **fabric,
	       void *context)
{
	struct util_fabric *util_fabric;
	int ret;

	util_fabric = calloc(1, sizeof(*util_fabric));
	if (!util_fabric)
		return -FI_ENOMEM;

	ret = ofi_fabric_init(&rds_prov, rds_info.fabric_attr, attr,
			      util_fabric, context);
	if (ret) {
		free(util_fabric);
		return ret;
	}

	*fabric = &util_fabric->fabric_fid;
	(*fabric)->fid.ops = &rds_fabric_fi_ops;
	(*fabric)->ops = &rds_fabric_ops;
	return 0;
}

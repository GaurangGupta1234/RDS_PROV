/*
 * libfabric RDS provider - provider registration and fi_getinfo.
 *
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 */

#include <rdma/fi_errno.h>
#include <ofi_prov.h>
#include "rds.h"

size_t rds_eager_size = RDS_DEF_EAGER_SIZE;

static int rds_getinfo(uint32_t version, const char *node, const char *service,
		       uint64_t flags, const struct fi_info *hints,
		       struct fi_info **info)
{
	int ret;

	/*
	 * Reuse the core IP getinfo helper: it fills in src/dest sockaddr_in
	 * from node/service, validates hints against rds_util_prov, and clones
	 * our template fi_info.  RDS addressing *is* IP addressing (the RDS
	 * socket is bound to a local RoCE/IPoIB interface address).
	 */
	ret = ofi_ip_getinfo(&rds_util_prov, version, node, service, flags,
			     hints, info);
	if (ret)
		return ret;

	return 0;
}

static void rds_fini(void)
{
	/* nothing global to tear down */
}

struct fi_provider rds_prov = {
	.name		= RDS_PROV_NAME,
	.version	= OFI_VERSION_DEF_PROV,
	.fi_version	= OFI_VERSION_LATEST,
	.getinfo	= rds_getinfo,
	.fabric		= rds_fabric,
	.cleanup	= rds_fini,
};

RDS_INI
{
	int val = RDS_DEF_EAGER_SIZE;

	fi_param_define(&rds_prov, "eager_size", FI_PARAM_INT,
			"Messages with payload <= this many bytes are sent "
			"eagerly (inline, one copy through the kernel). Larger "
			"messages use zero-copy RDMA-read rendezvous. "
			"(default: %d)", RDS_DEF_EAGER_SIZE);

	if (fi_param_get_int(&rds_prov, "eager_size", &val) == FI_SUCCESS) {
		if (val < 0)
			val = 0;
		if (val > RDS_MAX_EAGER_SIZE)
			val = RDS_MAX_EAGER_SIZE;
		rds_eager_size = (size_t) val;
	}

	return &rds_prov;
}

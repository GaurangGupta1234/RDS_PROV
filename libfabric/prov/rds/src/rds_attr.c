/*
 * libfabric RDS provider - capability / attribute tables.
 *
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 */

#include "rds.h"

/*
 * Capability set.  FI_ATOMICS / FI_MULTI_RECV are advertised so Intel MPI's
 * CH4/OFI strict capability check at MPI_Init accepts the provider; the runtime
 * is expected to disable the unimplemented features
 * (MPIR_CVAR_CH4_OFI_ENABLE_ATOMICS=0, ENABLE_RMA=0).  If a consumer actually
 * issues an atomic, the (stub) atomic ops return -FI_ENOSYS rather than
 * dereferencing a NULL table.  See docs/RDS_PROVIDER_DESIGN.md "Intel MPI".
 */
#define RDS_TX_CAPS	(FI_MSG | FI_TAGGED | FI_RMA | FI_SEND |	\
			 FI_READ | FI_WRITE | FI_ATOMICS)
#define RDS_RX_CAPS	(FI_MSG | FI_TAGGED | FI_RMA | FI_RECV |	\
			 FI_REMOTE_READ | FI_REMOTE_WRITE |		\
			 FI_DIRECTED_RECV | FI_SOURCE | FI_MULTI_RECV |	\
			 FI_ATOMICS)
#define RDS_DOMAIN_CAPS	(FI_LOCAL_COMM | FI_REMOTE_COMM | FI_ATOMICS)

/*
 * Message ordering: RDS delivers reliably and in order per (src,dst) pair, so
 * we can honour FI_ORDER_SAS (sends matched in send order) which MPI tag
 * matching depends on.  Completion ordering is intentionally NONE: the
 * rendezvous path completes asynchronously when the RDMA-read notification
 * arrives, which can race ahead of / behind a later eager op.  Each completion
 * carries its own op_context, so consumers (MPICH) handle this correctly.
 */
#define RDS_MSG_ORDER	(FI_ORDER_SAS | FI_ORDER_RAR | FI_ORDER_RAW |	\
			 FI_ORDER_WAR | FI_ORDER_WAW | FI_ORDER_SAW |	\
			 FI_ORDER_SAR | FI_ORDER_ATOMIC_RAR |		\
			 FI_ORDER_ATOMIC_RAW | FI_ORDER_ATOMIC_WAR |	\
			 FI_ORDER_ATOMIC_WAW)

struct fi_tx_attr rds_tx_attr = {
	.caps		= RDS_TX_CAPS,
	.op_flags	= FI_COMPLETION | FI_DELIVERY_COMPLETE | FI_INJECT |
			  FI_INJECT_COMPLETE,
	.msg_order	= RDS_MSG_ORDER,
	.comp_order	= FI_ORDER_NONE,
	.inject_size	= RDS_DEF_EAGER_SIZE,
	.size		= 1024,
	.iov_limit	= RDS_IOV_LIMIT,
	.rma_iov_limit	= 1,
};

struct fi_rx_attr rds_rx_attr = {
	.caps		= RDS_RX_CAPS,
	.op_flags	= FI_COMPLETION | FI_DELIVERY_COMPLETE | FI_INJECT |
			  FI_INJECT_COMPLETE | FI_MULTI_RECV,
	.msg_order	= RDS_MSG_ORDER,
	.comp_order	= FI_ORDER_NONE,
	.size		= 1024,
	.iov_limit	= RDS_IOV_LIMIT,
};

struct fi_ep_attr rds_ep_attr = {
	.type			= FI_EP_RDM,
	.protocol		= FI_PROTO_UNSPEC,
	.protocol_version	= 1,
	.max_msg_size		= RDS_MAX_MSG_SIZE,
	.mem_tag_format		= FI_TAG_GENERIC,
	.tx_ctx_cnt		= 1,
	.rx_ctx_cnt		= 1,
	.max_order_raw_size	= RDS_KERNEL_SEG_MAX,
	.max_order_war_size	= RDS_KERNEL_SEG_MAX,
	.max_order_waw_size	= RDS_KERNEL_SEG_MAX,
};

struct fi_domain_attr rds_domain_attr = {
	.name			= RDS_PROV_NAME,
	.caps			= RDS_DOMAIN_CAPS,
	.threading		= FI_THREAD_SAFE,
	.control_progress	= FI_PROGRESS_MANUAL,
	.data_progress		= FI_PROGRESS_MANUAL,
	.resource_mgmt		= FI_RM_ENABLED,
	.av_type		= FI_AV_UNSPEC,
	/*
	 * Scalable MR semantics: the provider mints the key (it *is* the RDS
	 * cookie) and RMA target addresses are offsets from the MR start --
	 * which is exactly what rds_rdma_args.remote_vec.addr expects.  We
	 * deliberately do NOT set FI_MR_VIRT_ADDR or FI_MR_LOCAL: local RDMA
	 * buffers are pinned by the kernel on demand and need no registration.
	 */
	.mr_mode		= FI_MR_BASIC,
	.mr_key_size		= sizeof(uint64_t),
	.cq_data_size		= 8,
	.cq_cnt			= 256,
	.ep_cnt			= 256,
	.tx_ctx_cnt		= 256,
	.rx_ctx_cnt		= 256,
	.max_ep_tx_ctx		= 1,
	.max_ep_rx_ctx		= 1,
	.mr_cnt			= 65536,
};

struct fi_fabric_attr rds_fabric_attr = {
	.name		= "RDS",
	.prov_version	= OFI_VERSION_DEF_PROV,
};

struct fi_info rds_info = {
	.caps		= RDS_DOMAIN_CAPS | RDS_TX_CAPS | RDS_RX_CAPS,
	.addr_format	= FI_SOCKADDR_IN,
	.tx_attr	= &rds_tx_attr,
	.rx_attr	= &rds_rx_attr,
	.ep_attr	= &rds_ep_attr,
	.domain_attr	= &rds_domain_attr,
	.fabric_attr	= &rds_fabric_attr,
};

struct util_prov rds_util_prov = {
	.prov	= &rds_prov,
	.info	= &rds_info,
	.flags	= 0,
};

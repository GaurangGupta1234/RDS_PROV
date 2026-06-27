/*
 * libfabric RDS provider - progress engine.
 *
 * Driven from ofi_cq_progress() (i.e. on every fi_cq_read).  We drain the RDS
 * socket with non-blocking recvmsg and dispatch each item:
 *
 *   - data datagrams    -> rds_handle_inbound() (EAGER / RTS / FIN framing)
 *   - RDS_CMSG_RDMA_STATUS control messages -> rds_handle_rdma_notify(): these
 *     are the asynchronous completions for our RMA ops and for the RDMA READs
 *     issued on the rendezvous receive path (matched by user_token).
 *
 * A single recvmsg can surface payload, one-or-more completion notifications,
 * or both, so we always walk the control buffer and then look at the data.
 *
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 */

#include <stdlib.h>
#include "rds.h"

/* Bound the work per progress call so one busy endpoint can't starve others
 * sharing the CQ. */
#define RDS_PROGRESS_BUDGET	256

static void rds_reap_notifies(struct rds_ep *ep, struct msghdr *msg)
{
	struct cmsghdr *cmsg;

	for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
		struct rds_rdma_notify notify;

		if (cmsg->cmsg_level != SOL_RDS ||
		    cmsg->cmsg_type != RDS_CMSG_RDMA_STATUS)
			continue;

		memcpy(&notify, CMSG_DATA(cmsg), sizeof(notify));
		rds_handle_rdma_notify(ep, notify.user_token, notify.status);
	}
}

void rds_ep_progress(struct util_ep *util_ep)
{
	struct rds_ep *ep;
	struct msghdr msg;
	struct iovec iov;
	union ofi_sock_ip src;
	fi_addr_t src_fi;
	ssize_t ret;
	int budget = RDS_PROGRESS_BUDGET;

	ep = container_of(util_ep, struct rds_ep, util_ep);
	if (ep->sock < 0)
		return;

	ofi_genlock_lock(&ep->util_ep.lock);

	/*
	 * Retry anything the socket previously rejected (parked FINs and
	 * rendezvous READs) before generating new work.  This drains the
	 * back-pressure queue in order and is what turns a momentarily full
	 * send queue into orderly progress instead of a dropped FIN/read.
	 */
	rds_progress_deferred(ep);

	/* Fast path: poll the memory-mapped eager rings (no syscall). */
	rds_ring_progress(ep);

	while (budget--) {
		iov.iov_base = ep->rx_buf;
		iov.iov_len = ep->rx_buf_size;

		memset(&msg, 0, sizeof(msg));
		msg.msg_name = &src;
		msg.msg_namelen = sizeof(src);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = ep->cmsg_buf;
		msg.msg_controllen = ep->cmsg_size;

		ret = recvmsg(ep->sock, &msg, MSG_DONTWAIT);
		if (ret < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				FI_WARN(&rds_prov, FI_LOG_EP_DATA,
					"recvmsg failed: %s\n",
					strerror(errno));
			break;
		}

		/* Completion notifications first (they may ride a 0-byte msg). */
		if (msg.msg_controllen)
			rds_reap_notifies(ep, &msg);

		if (msg.msg_flags & MSG_TRUNC) {
			FI_WARN(&rds_prov, FI_LOG_EP_DATA,
				"oversized datagram (%zd bytes) truncated; "
				"dropping\n", ret);
			continue;
		}

		if (ret < (ssize_t) sizeof(struct rds_hdr))
			continue;	/* no data payload (notify-only, etc.) */

		src_fi = ofi_ip_av_get_fi_addr(ep->util_ep.av, &src);
		rds_handle_inbound(ep, (struct rds_hdr *) ep->rx_buf,
				   ep->rx_buf + sizeof(struct rds_hdr),
				   (size_t) ret - sizeof(struct rds_hdr),
				   &src, src_fi);
	}

	ofi_genlock_unlock(&ep->util_ep.lock);
}

/*
 * libfabric RDS provider - eager RDMA ring fast path (opt-in).
 *
 * The RDS datagram receive path is the latency floor (~35us small-message RTT,
 * slower than TCP) because every message traverses the kernel datagram delivery
 * machinery.  This module bypasses it: each peer pair pre-registers a small
 * receive ring once, and small messages are placed with a single one-sided RDMA
 * write straight into the peer's ring.  The receiver detects arrival by polling
 * memory -- no per-message recvmsg, no interrupt.  This is the RDS-RDMA-V2
 * technique (~14us RTT) generalised to many peers and fed into the tag-matching
 * engine, so it beats TCP for small messages.
 *
 * Enabled with FI_RDS_EAGER_RDMA=1.  The RDS datagram eager/rendezvous paths
 * remain the default and the fallback (handshake in progress, ring full, or
 * peer count past the LRU/ resident cap).
 *
 * Memory: each peer ring costs slots*slot_size (registered) + the same again
 * for the local staging ring (not registered).  Defaults 16*1024 => 16 KiB +
 * 16 KiB per peer.  ring_max_peers caps how many rings are resident so pinned
 * memory stays bounded at scale; peers past the cap use the datagram path.
 *
 * Flow control: credit is piggybacked on reverse-direction data (slot.ack) and,
 * for one-directional streams, returned explicitly by RDMA-writing the consumer
 * index into the peer's credit cell every slots/2 slots.
 *
 * Ordering assumption: a single RDMA write lands in ascending address order, so
 * the per-slot trailing generation word is the last byte placed -- the same
 * assumption rds_rdma_v2.c relies on and which holds on the target HCA.
 *
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 */

#include <stdlib.h>
#include "rds.h"
#include <ofi_iov.h>

#define RDS_RING_PAGE	4096

/* ------------------------------------------------------------------ */
/* peer table						      */
/* ------------------------------------------------------------------ */

static unsigned rds_peer_hash(fi_addr_t addr)
{
	return (unsigned) ((addr * 2654435761u)) & (RDS_PEER_HASH_SIZE - 1);
}

static struct rds_peer *rds_peer_find(struct rds_ep *ep, fi_addr_t addr)
{
	struct dlist_entry *item;
	struct rds_peer *peer;

	dlist_foreach(&ep->peer_hash[rds_peer_hash(addr)], item) {
		peer = container_of(item, struct rds_peer, bucket);
		if (peer->addr == addr)
			return peer;
	}
	return NULL;
}

static void rds_peer_free(struct rds_ep *ep, struct rds_peer *peer)
{
	dlist_remove(&peer->bucket);
	dlist_remove(&peer->lru);
	ep->peer_cnt--;

	rds_mr_put(peer->rx_ring_mr);
	rds_mr_put(peer->credit_mr);
	free(peer->rx_ring);
	free(peer->tx_stage);
	free((void *) peer->credit_cell);
	free(peer);
}

void rds_ep_ring_init(struct rds_ep *ep)
{
	int i;

	ep->eager_rdma = rds_eager_rdma;
	ep->ring_slots = (uint32_t) rds_ring_slots;
	ep->ring_slot_size = (uint32_t) rds_ring_slot_size;
	ep->ring_max_peers = (uint32_t) rds_ring_max_peers;
	ep->ring_payload = rds_ring_payload_max(ep->ring_slot_size);

	for (i = 0; i < RDS_PEER_HASH_SIZE; i++)
		dlist_init(&ep->peer_hash[i]);
	dlist_init(&ep->peer_lru);
	ep->peer_cnt = 0;
}

void rds_ep_ring_cleanup(struct rds_ep *ep)
{
	struct dlist_entry *item, *tmp;
	struct rds_peer *peer;

	dlist_foreach_safe(&ep->peer_lru, item, tmp) {
		peer = container_of(item, struct rds_peer, lru);
		rds_peer_free(ep, peer);
	}
}

/* Allocate + register a peer's receive ring and credit cell. */
static struct rds_peer *rds_peer_create(struct rds_ep *ep, fi_addr_t addr)
{
	struct rds_peer *peer;
	size_t ring_bytes;
	const void *sa;

	if (ep->peer_cnt >= ep->ring_max_peers)
		return NULL;	/* hard cap: new peers use the datagram path */

	sa = rds_av_addr(ep, addr);
	if (!sa)
		return NULL;

	peer = calloc(1, sizeof(*peer));
	if (!peer)
		return NULL;

	peer->addr = addr;
	memcpy(&peer->sa, sa, ep->util_ep.av->addrlen);
	peer->state = RDS_PEER_NONE;

	ring_bytes = (size_t) ep->ring_slots * ep->ring_slot_size;

	if (posix_memalign((void **) &peer->rx_ring, RDS_RING_PAGE, ring_bytes))
		goto err;
	memset(peer->rx_ring, 0, ring_bytes);	/* gen stamps start at 0 */

	peer->tx_stage = malloc(ring_bytes);
	if (!peer->tx_stage)
		goto err;

	if (posix_memalign((void **) &peer->credit_cell, RDS_RING_PAGE,
			   sizeof(uint64_t)))
		goto err;
	*peer->credit_cell = 0;

	peer->rx_ring_mr = rds_reg_internal(ep, peer->rx_ring, ring_bytes,
					    FI_REMOTE_WRITE);
	peer->credit_mr = rds_reg_internal(ep, (void *) peer->credit_cell,
					   sizeof(uint64_t), FI_REMOTE_WRITE);
	if (!peer->rx_ring_mr || !peer->credit_mr)
		goto err;

	dlist_insert_head(&peer->bucket, &ep->peer_hash[rds_peer_hash(addr)]);
	dlist_insert_head(&peer->lru, &ep->peer_lru);
	ep->peer_cnt++;
	return peer;

err:
	rds_mr_put(peer->rx_ring_mr);
	rds_mr_put(peer->credit_mr);
	free(peer->rx_ring);
	free(peer->tx_stage);
	free((void *) peer->credit_cell);
	free(peer);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* handshake						      */
/* ------------------------------------------------------------------ */

static void rds_ring_send_ctrl(struct rds_ep *ep, struct rds_peer *peer,
			       uint8_t op)
{
	struct rds_hdr hdr;
	struct rds_ring_info info;
	struct iovec iov[2];
	struct msghdr msg;

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = RDS_HDR_MAGIC;
	hdr.op = op;
	hdr.size = sizeof(info);

	info.ring_cookie = (uint64_t) peer->rx_ring_mr->cookie;
	info.credit_cookie = (uint64_t) peer->credit_mr->cookie;
	info.slots = ep->ring_slots;
	info.slot_size = ep->ring_slot_size;

	iov[0].iov_base = &hdr;
	iov[0].iov_len = sizeof(hdr);
	iov[1].iov_base = &info;
	iov[1].iov_len = sizeof(info);

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &peer->sa;
	msg.msg_namelen = ofi_sizeofaddr((const struct sockaddr *) &peer->sa);
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;

	(void) sendmsg(ep->sock, &msg, 0);
}

/* Adopt the peer's published ring geometry; returns 0 if compatible. */
static int rds_ring_adopt(struct rds_ep *ep, struct rds_peer *peer,
			  const struct rds_ring_info *info)
{
	if (info->slots != ep->ring_slots ||
	    info->slot_size != ep->ring_slot_size) {
		FI_INFO(&rds_prov, FI_LOG_EP_CTRL,
			"peer ring geometry mismatch (%u/%u vs %u/%u); using "
			"datagram path for this peer\n", info->slots,
			info->slot_size, ep->ring_slots, ep->ring_slot_size);
		return -FI_EINVAL;
	}
	peer->rem_ring_cookie = (rds_rdma_cookie_t) info->ring_cookie;
	peer->rem_credit_cookie = (rds_rdma_cookie_t) info->credit_cookie;
	peer->rem_slots = info->slots;
	peer->rem_slot_size = info->slot_size;
	return 0;
}

void rds_ring_handle_ctrl(struct rds_ep *ep, struct rds_hdr *hdr, void *payload,
			  size_t plen, const union ofi_sock_ip *src,
			  fi_addr_t src_fi)
{
	struct rds_ring_info info;
	struct rds_peer *peer;

	if (!ep->eager_rdma || plen < sizeof(info) ||
	    src_fi == FI_ADDR_NOTAVAIL)
		return;
	memcpy(&info, payload, sizeof(info));

	peer = rds_peer_find(ep, src_fi);
	if (!peer) {
		peer = rds_peer_create(ep, src_fi);
		if (!peer)
			return;		/* at cap; peer keeps using datagrams */
	}

	if (rds_ring_adopt(ep, peer, &info))
		return;

	/* A REQ always warrants an ACK so the requester learns our geometry;
	 * an ACK completes our side. Either way we are now READY. */
	if (hdr->op == RDS_OP_RING_REQ)
		rds_ring_send_ctrl(ep, peer, RDS_OP_RING_ACK);
	peer->state = RDS_PEER_READY;
}

/* ------------------------------------------------------------------ */
/* send						      */
/* ------------------------------------------------------------------ */

static void rds_ring_refresh_credit(struct rds_peer *peer)
{
	uint64_t c = *peer->credit_cell;	/* monotonic, 8B atomic */

	if (c > peer->tx_tail)
		peer->tx_tail = c;
}

ssize_t rds_ring_send(struct rds_ep *ep, const struct iovec *iov,
		      size_t iov_count, fi_addr_t dest, uint64_t tag,
		      uint64_t data, void *context, uint64_t flags, int tagged,
		      int gen_comp)
{
	struct rds_peer *peer;
	struct rds_ring_slot *slot;
	char *stage;
	size_t total, idx;
	uint32_t gen;
	uint64_t remote_off;
	struct iovec liov;
	ssize_t ret;

	/*
	 * Return convention: -FI_ENOSYS means "ring not applicable, use the
	 * datagram path"; -FI_EAGAIN means "ring is READY but momentarily busy,
	 * propagate to the caller to retry".  The distinction matters for
	 * ordering: once a peer is on the ring we must NOT silently spill a
	 * later message onto the (separate) datagram channel, or it could
	 * overtake an earlier ring message and break FI_ORDER_SAS.
	 */
	if (!ep->eager_rdma)
		return -FI_ENOSYS;

	total = ofi_total_iov_len(iov, iov_count);
	if (total > ep->ring_payload)
		return -FI_ENOSYS;		/* too big -> datagram/rndzv */

	peer = rds_peer_find(ep, dest);
	if (!peer) {
		peer = rds_peer_create(ep, dest);
		if (!peer)
			return -FI_ENOSYS;	/* at cap -> datagram (always) */
	}

	/* keep most-recently-used at the head (informational; no eviction) */
	dlist_remove(&peer->lru);
	dlist_insert_head(&peer->lru, &ep->peer_lru);

	if (peer->state != RDS_PEER_READY) {
		if (peer->state == RDS_PEER_NONE) {
			rds_ring_send_ctrl(ep, peer, RDS_OP_RING_REQ);
			peer->state = RDS_PEER_CONNECTING;
		}
		return -FI_ENOSYS;	/* handshake pending -> datagram now */
	}

	rds_ring_refresh_credit(peer);
	if (peer->tx_head - peer->tx_tail >= peer->rem_slots)
		return -FI_EAGAIN;	/* ring full -> retry (keep order) */

	idx = peer->tx_head % peer->rem_slots;
	gen = (uint32_t) (peer->tx_head / peer->rem_slots) + 1;
	remote_off = idx * peer->rem_slot_size;

	/* Stage the full slot locally (so the gen trailer is contiguous and
	 * lands last), then one RDMA write places it in the peer's ring. The
	 * staging slot is only reused once flow control says this slot index
	 * has been consumed, so no completion notification is needed. */
	stage = peer->tx_stage + idx * ep->ring_slot_size;
	slot = (struct rds_ring_slot *) stage;
	slot->len = (uint32_t) total;
	slot->flags = (tagged ? RDS_HF_TAGGED : 0) |
		      ((flags & FI_REMOTE_CQ_DATA) ? RDS_HF_CQ_DATA : 0);
	slot->tag = tag;
	slot->data = data;
	slot->ack = peer->rx_head;		/* piggyback reverse credit */
	ofi_copy_from_iov(rds_slot_payload(stage), total, iov, iov_count, 0);
	*rds_slot_gen(stage, ep->ring_slot_size) = gen;

	liov.iov_base = stage;
	liov.iov_len = ep->ring_slot_size;
	ret = rds_post_rdma(ep, &peer->sa,
			    ofi_sizeofaddr((const struct sockaddr *) &peer->sa),
			    peer->rem_ring_cookie, remote_off, &liov, 1,
			    1 /* write */, 0 /* no notify */, 0);
	if (ret)
		return ret;	/* -FI_EAGAIN bubbles up -> datagram fallback */

	peer->tx_head++;

	/* Eager completion: user buffer was copied into staging, so it is
	 * reusable now and RDS will deliver the write reliably. */
	if (gen_comp)
		rds_cq_write_tx(ep, context,
				FI_SEND | (tagged ? FI_TAGGED : FI_MSG),
				total, 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* receive (memory poll)					      */
/* ------------------------------------------------------------------ */

static void rds_ring_return_credit(struct rds_ep *ep, struct rds_peer *peer)
{
	struct iovec liov;

	peer->credit_tx = peer->rx_head;
	liov.iov_base = &peer->credit_tx;
	liov.iov_len = sizeof(peer->credit_tx);

	if (rds_post_rdma(ep, &peer->sa,
			  ofi_sizeofaddr((const struct sockaddr *) &peer->sa),
			  peer->rem_credit_cookie, 0, &liov, 1, 1 /* write */,
			  0 /* no notify */, 0) == 0)
		peer->rx_credit_sent = peer->rx_head;
}

void rds_ring_deliver(struct rds_ep *ep, struct rds_peer *peer,
		      struct rds_ring_slot *slot, void *payload)
{
	struct rds_hdr hdr;

	/* Re-use the datagram matching/delivery engine by synthesising the
	 * header that an eager datagram would have carried. */
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = RDS_HDR_MAGIC;
	hdr.op = RDS_OP_EAGER;
	hdr.flags = slot->flags;
	hdr.tag = slot->tag;
	hdr.data = slot->data;
	hdr.size = slot->len;

	rds_handle_inbound(ep, &hdr, payload, slot->len, &peer->sa, peer->addr);
}

static void rds_ring_poll_peer(struct rds_ep *ep, struct rds_peer *peer)
{
	for (;;) {
		uint32_t idx = peer->rx_head % ep->ring_slots;
		uint32_t want = (uint32_t) (peer->rx_head / ep->ring_slots) + 1;
		char *slotp = peer->rx_ring + (size_t) idx * ep->ring_slot_size;
		struct rds_ring_slot *slot = (struct rds_ring_slot *) slotp;

		if (*rds_slot_gen(slotp, ep->ring_slot_size) != want)
			break;		/* no fully-arrived slot here */

		/* full slot present (gen written last); absorb piggybacked
		 * credit for our own sends, then deliver. */
		if (slot->ack > peer->tx_tail)
			peer->tx_tail = slot->ack;

		rds_ring_deliver(ep, peer, slot, rds_slot_payload(slotp));
		peer->rx_head++;

		/*
		 * Return credit in batches for one-directional streams.  Bidir
		 * traffic (ping-pong, collectives) needs no explicit credit at
		 * all: the reverse data carries it in slot.ack, so the common
		 * case costs zero extra RDMA.
		 */
		if (peer->rx_head - peer->rx_credit_sent >= ep->ring_slots / 2)
			rds_ring_return_credit(ep, peer);
	}
}

void rds_ring_progress(struct rds_ep *ep)
{
	struct dlist_entry *item;
	struct rds_peer *peer;

	if (!ep->eager_rdma || !ep->peer_cnt)
		return;

	dlist_foreach(&ep->peer_lru, item) {
		peer = container_of(item, struct rds_peer, lru);
		if (peer->state == RDS_PEER_READY)
			rds_ring_poll_peer(ep, peer);
	}
}

/* SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
 * Copyright(c) 2018-2020 Pensando Systems, Inc. All rights reserved.
 */

#include <sys/queue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <inttypes.h>

#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ethdev_driver.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_sctp.h>
#include <rte_string_fns.h>
#include <rte_errno.h>
#include <rte_ip.h>
#include <rte_net.h>
#include <rte_prefetch.h>

#include "ionic.h"
#include "ionic_ethdev.h"
#include "ionic_lif.h"
#include "ionic_rxtx.h"

static __rte_always_inline void
ionic_tx_flush_sg(struct ionic_tx_qcq *txq)
{
	struct ionic_cq *cq = &txq->qcq.cq;
	struct ionic_queue *q = &txq->qcq.q;
	struct ionic_tx_stats *stats = &txq->stats;
#ifdef IONIC_CODE_PERF_TX
	uint64_t start_cycles = ionic_tsc(), flush_cycles, avg_cycles;
	uint32_t cnt = 0;
#endif
	struct rte_mbuf *txm;
	struct ionic_txq_comp *cq_desc, *cq_desc_base = cq->base;
	void **info;
	uint32_t i;

	cq_desc = &cq_desc_base[cq->tail_idx];
	DPDK_SIM_DESC_RD(struct ionic_txq_comp,
			cq->base_pa, cq->tail_idx, cq_desc);

	while (color_match(cq_desc->color, cq->done_color)) {
		cq->tail_idx = Q_NEXT_TO_SRVC(cq, 1);
		if (unlikely(cq->tail_idx == 0))
			cq->done_color = !cq->done_color;

#ifdef IONIC_PREFETCH
		/* Prefetch 4 x 16B comp at cq->tail_idx + 4 */
		if ((cq->tail_idx & 0x3) == 0)
			rte_prefetch0(&cq_desc_base[Q_NEXT_TO_SRVC(cq, 4)]);
#endif

		while (q->tail_idx != rte_le_to_cpu_16(cq_desc->comp_index)) {
#ifdef IONIC_PREFETCH
			/* Prefetch 8 mbuf ptrs at q->tail_idx + 2 */
			rte_prefetch0(IONIC_INFO_PTR(q, Q_NEXT_TO_SRVC(q, 2)));

			/* Prefetch next mbuf */
			void **next_info =
				IONIC_INFO_PTR(q, Q_NEXT_TO_SRVC(q, 1));
			if (next_info[0])
				rte_mbuf_prefetch_part2(next_info[0]);
			if (next_info[1])
				rte_mbuf_prefetch_part2(next_info[1]);
#endif

			info = IONIC_INFO_PTR(q, q->tail_idx);
			for (i = 0; i < q->num_segs; i++) {
				txm = info[i];
				if (!txm)
					break;

				DPDK_SIM_TX_DONE(txm);

				if (txq->flags & IONIC_QCQ_F_FAST_FREE)
					rte_mempool_put(txm->pool, txm);
				else
					rte_pktmbuf_free_seg(txm);

				info[i] = NULL;
			}

			q->tail_idx = Q_NEXT_TO_SRVC(q, 1);
#ifdef IONIC_CODE_PERF_TX
			cnt++;
#endif
		}

		cq_desc = &cq_desc_base[cq->tail_idx];
		stats->comps++;
	}
#ifdef IONIC_CODE_PERF_TX
	if (cnt > 0) {
		flush_cycles = ionic_tsc() - start_cycles;
		avg_cycles = flush_cycles / cnt;
		stats->tx_flush_cycles += flush_cycles;
		if (avg_cycles < stats->tx_flush_min || !stats->tx_flush_min)
			stats->tx_flush_min = avg_cycles;
		if (avg_cycles > stats->tx_flush_max)
			stats->tx_flush_max = avg_cycles;
	}
#endif
}

static __rte_always_inline int
ionic_tx_sg(struct ionic_tx_qcq *txq, struct rte_mbuf *txm)
{
	struct ionic_queue *q = &txq->qcq.q;
	struct ionic_txq_desc *desc, *desc_base = q->base;
	struct ionic_txq_sg_desc_v1 *sg_desc, *sg_desc_base = q->sg_base;
	struct ionic_txq_sg_elem *elem;
	struct ionic_tx_stats *stats = &txq->stats;
	struct rte_mbuf *txm_seg;
	rte_iova_t data_iova;
	void **info;
	uint64_t ol_flags = txm->ol_flags;
	uint64_t addr, cmd;
	uint8_t opcode = IONIC_TXQ_DESC_OPCODE_CSUM_NONE;
	uint8_t flags = 0;

	desc = &desc_base[q->head_idx];
	sg_desc = &sg_desc_base[q->head_idx];
	info = IONIC_INFO_PTR(q, q->head_idx);

	if ((ol_flags & PKT_TX_IP_CKSUM) &&
	    (txq->flags & IONIC_QCQ_F_CSUM_L3)) {
		opcode = IONIC_TXQ_DESC_OPCODE_CSUM_HW;
		flags |= IONIC_TXQ_DESC_FLAG_CSUM_L3;
	}

	if (((ol_flags & PKT_TX_TCP_CKSUM) &&
	     (txq->flags & IONIC_QCQ_F_CSUM_TCP)) ||
	    ((ol_flags & PKT_TX_UDP_CKSUM) &&
	     (txq->flags & IONIC_QCQ_F_CSUM_UDP))) {
		opcode = IONIC_TXQ_DESC_OPCODE_CSUM_HW;
		flags |= IONIC_TXQ_DESC_FLAG_CSUM_L4;
	}

	if (opcode == IONIC_TXQ_DESC_OPCODE_CSUM_NONE)
		stats->no_csum++;

	if (((ol_flags & PKT_TX_OUTER_IP_CKSUM) ||
	     (ol_flags & PKT_TX_OUTER_UDP_CKSUM)) &&
	    ((ol_flags & PKT_TX_OUTER_IPV4) ||
	     (ol_flags & PKT_TX_OUTER_IPV6))) {
		flags |= IONIC_TXQ_DESC_FLAG_ENCAP;
	}

	if (ol_flags & PKT_TX_VLAN_PKT) {
		flags |= IONIC_TXQ_DESC_FLAG_VLAN;
		desc->vlan_tci = rte_cpu_to_le_16(txm->vlan_tci);
	}

	addr = rte_cpu_to_le_64(rte_mbuf_data_iova(txm));

	DPDK_SIM_TX(txm, addr);

	cmd = encode_txq_desc_cmd(opcode, flags, txm->nb_segs - 1, addr);
	desc->cmd = rte_cpu_to_le_64(cmd);
	desc->len = rte_cpu_to_le_16(txm->data_len);

	info[0] = txm;

	if (unlikely(txm->nb_segs > 1)) {
		txm_seg = txm->next;

		elem = sg_desc->elems;

		while (txm_seg != NULL) {
			/* Stash the mbuf ptr in the array */
			info++;
			*info = txm_seg;

			/* Configure the SGE */
			data_iova = rte_mbuf_data_iova(txm_seg);
			elem->len = rte_cpu_to_le_16(txm_seg->data_len);
			elem->addr = rte_cpu_to_le_64(data_iova);
			DPDK_SIM_TX(txm_seg, elem->addr);
			elem++;

			txm_seg = txm_seg->next;
		}
	}

	DPDK_SIM_DESC_WR(struct ionic_txq_desc, q->base_pa, q->head_idx, desc);
	DPDK_SIM_DESC_WR(struct ionic_txq_sg_desc_v1,
			q->sg_base_pa, q->head_idx, sg_desc);

	q->head_idx = Q_NEXT_TO_POST(q, 1);

	return 0;
}

uint16_t
ionic_xmit_pkts_sg(void *tx_queue, struct rte_mbuf **tx_pkts,
		uint16_t nb_pkts)
{
#ifdef IONIC_CODE_PERF_TX
	uint64_t start_cycles, post_cycles, avg_cycles;
#endif
	struct ionic_tx_qcq *txq = tx_queue;
	struct ionic_queue *q = &txq->qcq.q;
	struct ionic_tx_stats *stats = &txq->stats;
	struct rte_mbuf *mbuf;
	uint32_t bytes_tx = 0;
	uint16_t nb_avail, nb_tx = 0;
	uint64_t then, now, hz, delta;
	int err;

#ifdef IONIC_PREFETCH
	struct ionic_txq_desc *desc_base = q->base;
#ifdef IONIC_EMBEDDED
	rte_prefetch0(&desc_base[q->head_idx]);
#else
	if (!(txq->flags & IONIC_QCQ_F_CMB))
		rte_prefetch0(&desc_base[q->head_idx]);
#endif
	rte_prefetch0(IONIC_INFO_PTR(q, q->head_idx));

	if (tx_pkts) {
		rte_mbuf_prefetch_part1(tx_pkts[0]);
		rte_mbuf_prefetch_part2(tx_pkts[0]);
	}
#endif

	if (unlikely(ionic_q_space_avail(q) < txq->free_thresh)) {
		/* Cleaning old buffers */
		ionic_tx_flush_sg(txq);
	}

#ifdef IONIC_CODE_PERF_TX
	start_cycles = ionic_tsc();
#endif

	nb_avail = ionic_q_space_avail(q);
	if (unlikely(nb_avail < nb_pkts)) {
		stats->stop += nb_pkts - nb_avail;
		nb_pkts = nb_avail;
	}

	while (nb_tx < nb_pkts) {
#ifdef IONIC_PREFETCH
		uint16_t next_idx = Q_NEXT_TO_POST(q, 1);
#ifdef IONIC_EMBEDDED
		rte_prefetch0(&desc_base[next_idx]);
#else
		if (!(txq->flags & IONIC_QCQ_F_CMB))
			rte_prefetch0(&desc_base[next_idx]);
#endif
		rte_prefetch0(IONIC_INFO_PTR(q, next_idx));

		if (nb_tx + 1 < nb_pkts) {
			rte_mbuf_prefetch_part1(tx_pkts[nb_tx + 1]);
			rte_mbuf_prefetch_part2(tx_pkts[nb_tx + 1]);
		}
#endif

		mbuf = tx_pkts[nb_tx];

		if (mbuf->ol_flags & PKT_TX_TCP_SEG)
			err = ionic_tx_tso(txq, mbuf);
		else
			err = ionic_tx_sg(txq, mbuf);
		if (unlikely(err)) {
			stats->drop += nb_pkts - nb_tx;
			break;
		}

		bytes_tx += mbuf->pkt_len;
		nb_tx++;
	}

	if (nb_tx > 0) {
		rte_wmb();
		ionic_q_flush(q);

		txq->last_wdog_cycles = rte_get_timer_cycles();

#ifdef IONIC_CODE_PERF_TX
		post_cycles = ionic_tsc() - start_cycles;
		avg_cycles = post_cycles / nb_tx;
		stats->tx_post_cycles += post_cycles;
		if (avg_cycles < stats->tx_post_min || !stats->tx_post_min)
			stats->tx_post_min = avg_cycles;
		if (avg_cycles > stats->tx_post_max)
			stats->tx_post_max = avg_cycles;
#endif

		stats->packets += nb_tx;
		stats->bytes += bytes_tx;
	} else {
		/*
		 * Ring the doorbell again if no work could be posted and work
		 * is still pending after the deadline.
		 */
		if (q->head_idx != q->tail_idx) {
			then = txq->last_wdog_cycles;
			now = rte_get_timer_cycles();
			hz = rte_get_timer_hz();
			delta = (now - then) * 1000;

			if (delta >= hz * IONIC_Q_WDOG_MS) {
				ionic_q_flush(q);
				txq->last_wdog_cycles = now;
			}
		}
	}

	return nb_tx;
}

/*
 * Cleans one descriptor. Connects the filled mbufs into a chain.
 * Does not advance the tail index.
 */
static __rte_always_inline void
ionic_rx_clean_one_sg(struct ionic_rx_qcq *rxq,
		struct ionic_rxq_comp *cq_desc,
		struct ionic_rx_service *rx_svc)
{
	struct ionic_queue *q = &rxq->qcq.q;
	struct rte_mbuf *rxm;
	struct rte_mbuf *rxm_seg, *prev_rxm;
	struct ionic_rx_stats *stats = &rxq->stats;
	uint64_t pkt_flags = 0;
	uint32_t pkt_type;
	uint32_t left, i;
	uint16_t cq_desc_len;
	uint8_t ptype, cflags;
	void **info;

	cq_desc_len = rte_le_to_cpu_16(cq_desc->len);

	assert(q->tail_idx == rte_le_to_cpu_16(cq_desc->comp_index));

	info = IONIC_INFO_PTR(q, q->tail_idx);

	rxm = info[0];

	if (unlikely(cq_desc->status)) {
		stats->bad_cq_status++;
		return;
	}

	if (unlikely(cq_desc_len > rxq->frame_size || cq_desc_len == 0)) {
		stats->bad_len++;
		return;
	}

	info[0] = NULL;

	/* Set the mbuf metadata based on the cq entry */
	rxm->rearm_data[0] = rxq->rearm_data;
	rxm->pkt_len = cq_desc_len;
	rxm->data_len = RTE_MIN(rxq->hdr_seg_size, cq_desc_len);
	left = cq_desc_len - rxm->data_len;
	rxm->nb_segs = cq_desc->num_sg_elems + 1;

	DPDK_SIM_RX_DONE(rxm);

	prev_rxm = rxm;

	for (i = 1; i < rxm->nb_segs && left; i++) {
		rxm_seg = info[i];
		info[i] = NULL;

		/* Set the chained mbuf metadata */
		rxm_seg->rearm_data[0] = rxq->rearm_seg_data;
		rxm_seg->data_len = RTE_MIN(rxq->seg_size, left);
		left -= rxm_seg->data_len;

		/* Link the mbuf */
		prev_rxm->next = rxm_seg;
		prev_rxm = rxm_seg;

		DPDK_SIM_RX_DONE(rxm_seg);
	}

	/* Terminate the mbuf chain */
	prev_rxm->next = NULL;

	/* RSS */
	pkt_flags |= PKT_RX_RSS_HASH;
	rxm->hash.rss = rte_le_to_cpu_32(cq_desc->rss_hash);

	/* Vlan Strip */
	if (cq_desc->csum_flags & IONIC_RXQ_COMP_CSUM_F_VLAN) {
		pkt_flags |= PKT_RX_VLAN | PKT_RX_VLAN_STRIPPED;
		rxm->vlan_tci = rte_le_to_cpu_16(cq_desc->vlan_tci);
	}

	/* Checksum */
	if (cq_desc->csum_flags & IONIC_RXQ_COMP_CSUM_F_CALC) {
		cflags = cq_desc->csum_flags & IONIC_CSUM_FLAG_MASK;
		pkt_flags |= ionic_csum_flags[cflags];
	}

	rxm->ol_flags = pkt_flags;

	/* Packet Type */
	ptype = cq_desc->pkt_type_color & IONIC_RXQ_COMP_PKT_TYPE_MASK;
	pkt_type = ionic_ptype_table[ptype];
	if (unlikely(pkt_type == RTE_PTYPE_UNKNOWN)) {
		struct rte_ether_hdr *eth_h = rte_pktmbuf_mtod(rxm,
				struct rte_ether_hdr *);
		uint16_t ether_type = eth_h->ether_type;
		if (ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP))
			pkt_type = RTE_PTYPE_L2_ETHER_ARP;
		else if (ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_LLDP))
			pkt_type = RTE_PTYPE_L2_ETHER_LLDP;
		else if (ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_1588))
			pkt_type = RTE_PTYPE_L2_ETHER_TIMESYNC;
		stats->mtods++;
	} else if (pkt_flags & PKT_RX_VLAN) {
		pkt_type |= RTE_PTYPE_L2_ETHER_VLAN;
	} else {
		pkt_type |= RTE_PTYPE_L2_ETHER;
	}

	rxm->packet_type = pkt_type;

	rx_svc->rx_pkts[rx_svc->nb_rx] = rxm;
	rx_svc->nb_rx++;

	stats->packets++;
	stats->bytes += rxm->pkt_len;
}

/*
 * Fills one descriptor with mbufs. Does not advance the head index.
 */
static __rte_always_inline int
ionic_rx_fill_one_sg(struct ionic_rx_qcq *rxq)
{
	struct ionic_queue *q = &rxq->qcq.q;
	struct rte_mbuf *rxm;
	struct rte_mbuf *rxm_seg;
	struct ionic_rxq_desc *desc, *desc_base = q->base;
	struct ionic_rxq_sg_desc *sg_desc, *sg_desc_base = q->sg_base;
	rte_iova_t data_iova;
	uint32_t i;
	void **info;
	int ret;

	info = IONIC_INFO_PTR(q, q->head_idx);
	desc = &desc_base[q->head_idx];
	sg_desc = &sg_desc_base[q->head_idx];

	/* mbuf is unused => whole chain is unused */
	if (unlikely(info[0]))
		return 0;

	if (unlikely(rxq->hdr_idx == 0)) {
		ret = rte_mempool_get_bulk(rxq->mb_hdr_pool,
					(void **)rxq->hdrs,
					IONIC_MBUF_BULK_ALLOC);
		if (unlikely(ret))
			return -ENOMEM;

		rxq->hdr_idx = IONIC_MBUF_BULK_ALLOC;
	}

	rxm = rxq->hdrs[--rxq->hdr_idx];
	info[0] = rxm;

	data_iova = rte_mbuf_data_iova_default(rxm);
	DPDK_SIM_RX(rxm, data_iova, rxq->hdr_seg_size);
	desc->addr = rte_cpu_to_le_64(data_iova);

	for (i = 1; i < q->num_segs; i++) {
		/* mbuf is unused => rest of the chain is unused */
		if (info[i])
			return 0;

		if (unlikely(rxq->seg_idx == 0)) {
			ret = rte_mempool_get_bulk(rxq->mb_seg_pool,
					(void **)rxq->segs,
					IONIC_MBUF_BULK_ALLOC);
			if (unlikely(ret))
				return -ENOMEM;

			rxq->seg_idx = IONIC_MBUF_BULK_ALLOC;
		}

		rxm_seg = rxq->segs[--rxq->seg_idx];
		info[i] = rxm_seg;

		/* The data_off doesn't get set to 0 until later */
		data_iova = rxm_seg->buf_iova;
		DPDK_SIM_RX(rxm_seg, data_iova, rxq->seg_size);
		sg_desc->elems[i - 1].addr = rte_cpu_to_le_64(data_iova);
	}

	DPDK_SIM_DESC_WR(struct ionic_rxq_desc, q->base_pa, q->head_idx, desc);
	DPDK_SIM_DESC_WR(struct ionic_rxq_sg_desc,
			q->sg_base_pa, q->head_idx, sg_desc);

	return 0;
}

/*
 * Walk the CQ to find completed receive descriptors.
 * Any completed descriptor found is refilled.
 */
static __rte_always_inline void
ionic_rxq_service_sg(struct ionic_rx_qcq *rxq, uint32_t work_to_do,
		struct ionic_rx_service *rx_svc)
{
#ifdef IONIC_CODE_PERF_RX
	struct ionic_rx_stats *stats = &rxq->stats;
	uint64_t start_cycles = ionic_tsc(), service_cycles, avg_cycles;
#endif
	struct ionic_cq *cq = &rxq->qcq.cq;
	struct ionic_queue *q = &rxq->qcq.q;
#ifdef IONIC_PREFETCH
	struct ionic_rxq_desc *q_desc_base = q->base;
#endif
	struct ionic_rxq_comp *cq_desc, *cq_desc_base = cq->base;
	uint32_t work_done = 0;
	uint64_t then, now, hz, delta;
	int ret;

	cq_desc = &cq_desc_base[cq->tail_idx];
	DPDK_SIM_DESC_RD(struct ionic_rxq_comp,
			cq->base_pa, cq->tail_idx, cq_desc);

	while (color_match(cq_desc->pkt_type_color, cq->done_color)) {
		cq->tail_idx = Q_NEXT_TO_SRVC(cq, 1);
		if (unlikely(cq->tail_idx == 0))
			cq->done_color = !cq->done_color;

#ifdef IONIC_PREFETCH
		/* Prefetch 8 x 8B bufinfo */
		rte_prefetch0(IONIC_INFO_PTR(q, Q_NEXT_TO_SRVC(q, 8)));
		/* Prefetch 4 x 16B comp */
		rte_prefetch0(&cq_desc_base[Q_NEXT_TO_SRVC(cq, 4)]);
		/* Prefetch 4 x 16B descriptors */
#ifdef IONIC_EMBEDDED
		rte_prefetch0(&q_desc_base[Q_NEXT_TO_POST(q, 4)]);
#else
		if (!(rxq->flags & IONIC_QCQ_F_CMB))
			rte_prefetch0(&q_desc_base[Q_NEXT_TO_POST(q, 4)]);
#endif
#endif

		/* Clean one descriptor */
		ionic_rx_clean_one_sg(rxq, cq_desc, rx_svc);
		q->tail_idx = Q_NEXT_TO_SRVC(q, 1);

		/* Fill one descriptor */
		ret = ionic_rx_fill_one_sg(rxq);
		assert(!ret);
		q->head_idx = Q_NEXT_TO_POST(q, 1);

		if (unlikely(++work_done == work_to_do))
			break;

		cq_desc = &cq_desc_base[cq->tail_idx];
		DPDK_SIM_DESC_RD(struct ionic_rxq_comp,
				cq->base_pa, cq->tail_idx, cq_desc);
	}

	/* Update the queue indices and ring the doorbell */
	if (work_done) {
		ionic_q_flush(q);

		rxq->last_wdog_cycles = rte_get_timer_cycles();
		rxq->wdog_ms = IONIC_Q_WDOG_MS;

#ifdef IONIC_CODE_PERF_RX
		service_cycles = ionic_tsc() - start_cycles;
		avg_cycles = service_cycles / work_done;

		if (avg_cycles > stats->rx_service_max)
			stats->rx_service_max = avg_cycles;
		if (avg_cycles < stats->rx_service_min ||
		    !stats->rx_service_min)
			stats->rx_service_min = avg_cycles;
		stats->rx_service_cycles += service_cycles;
		stats->rx_calls++;
#endif
	} else {
		/*
		 * Ring the doorbell again if no recvs were posted and the
		 * recv queue is not empty after the deadline.
		 *
		 * Exponentially back off the deadline to avoid excessive
		 * doorbells when the recv queue is idle.
		 */
		if (q->head_idx != q->tail_idx) {
			then = rxq->last_wdog_cycles;
			now = rte_get_timer_cycles();
			hz = rte_get_timer_hz();
			delta = (now - then) * 1000;

			if (delta >= hz * rxq->wdog_ms) {
				ionic_q_flush(q);
				rxq->last_wdog_cycles = now;

				delta = 2 * rxq->wdog_ms;
				if (delta > IONIC_Q_WDOG_MAX_MS)
					delta = IONIC_Q_WDOG_MAX_MS;

				rxq->wdog_ms = delta;
			}
		}

#ifdef IONIC_CODE_PERF_RX
		stats->rx_calls_empty++;
#endif
	}
}

uint16_t
ionic_recv_pkts_sg(void *rx_queue, struct rte_mbuf **rx_pkts,
		uint16_t nb_pkts)
{
	struct ionic_rx_qcq *rxq = rx_queue;
	struct ionic_rx_service rx_svc;

	rx_svc.rx_pkts = rx_pkts;
	rx_svc.nb_rx = 0;

	ionic_rxq_service_sg(rxq, nb_pkts, &rx_svc);

	return rx_svc.nb_rx;
}

/*
 * Fills all descriptors with mbufs.
 */
int __rte_cold
ionic_rx_fill_sg(struct ionic_rx_qcq *rxq)
{
	struct ionic_queue *q = &rxq->qcq.q;
	uint32_t i;
	int err = 0;

	for (i = 0; i < q->num_descs - 1u; i++) {
		err = ionic_rx_fill_one_sg(rxq);
		if (err)
			break;

		q->head_idx = Q_NEXT_TO_POST(q, 1);
	}

	ionic_q_flush(q);

	return err;
}

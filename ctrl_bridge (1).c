/* SPDX-License-Identifier: BSD-3-Clause
 *
 * ctrl_bridge.c  --  Control-plane exception path (virtio-user) implementation.
 * See ctrl_bridge.h for the design overview.
 */

#include <string.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>

#include "ctrl_bridge.h"
#include "dbg.h"

#define CB_NB_RXD 256
#define CB_NB_TXD 256
#define CB_BURST  32

static int      g_ready    = 0;
static uint16_t g_ctrl_pid = 0;     /* virtio-user ethdev port id */
static uint16_t g_wan_pid  = 0;     /* encrypted WAN port (port 1) */
static struct rte_mempool *g_mp = NULL;

int cb_ready(void)            { return g_ready; }
uint16_t cb_ctrl_portid(void) { return g_ctrl_pid; }

/*
 * Locate the virtio-user port by its vdev name. DPDK assigns it an ethdev
 * port id when --vdev=virtio_user0,... is on the command line. We match on the
 * device name so we don't hard-code a port number (it lands after the two
 * physical ports, but we don't assume that).
 */
static int
cb_find_vdev_port(uint16_t *out)
{
	uint16_t pid;
	RTE_ETH_FOREACH_DEV(pid) {
		struct rte_eth_dev_info info;
		if (rte_eth_dev_info_get(pid, &info) != 0)
			continue;
		/* device name (rte_eth_dev) carries the vdev name */
		const char *nm = rte_dev_name(info.device);
		if (nm && strstr(nm, CB_VDEV_NAME) != NULL) {
			*out = pid;
			return 0;
		}
	}
	return -1;
}

int
cb_init(uint16_t wan_portid, struct rte_mempool *mp)
{
	uint16_t pid;
	int ret;

	g_ready = 0;
	g_wan_pid = wan_portid;
	g_mp = mp;

	if (cb_find_vdev_port(&pid) != 0) {
		DBG(DBG_INIT,
		    "ctrl_bridge: vdev '%s' not found — control bridge DISABLED "
		    "(did you pass --vdev=%s,path=/dev/vhost-net,iface=ctrl0,queues=1 ?)\n",
		    CB_VDEV_NAME, CB_VDEV_NAME);
		return -1;
	}

	/* Configure the virtio-user port: 1 rx + 1 tx queue, default conf. */
	struct rte_eth_conf pconf;
	memset(&pconf, 0, sizeof(pconf));

	ret = rte_eth_dev_configure(pid, 1, 1, &pconf);
	if (ret < 0) {
		DBG(DBG_INIT, "ctrl_bridge: configure(port %u) failed: %d\n", pid, ret);
		return -1;
	}

	uint16_t nb_rxd = CB_NB_RXD, nb_txd = CB_NB_TXD;
	ret = rte_eth_dev_adjust_nb_rx_tx_desc(pid, &nb_rxd, &nb_txd);
	if (ret < 0) {
		DBG(DBG_INIT, "ctrl_bridge: adjust_nb_desc(port %u) failed: %d\n", pid, ret);
		return -1;
	}

	ret = rte_eth_rx_queue_setup(pid, 0, nb_rxd,
				     rte_eth_dev_socket_id(pid), NULL, g_mp);
	if (ret < 0) {
		DBG(DBG_INIT, "ctrl_bridge: rx_queue_setup(port %u) failed: %d\n", pid, ret);
		return -1;
	}

	ret = rte_eth_tx_queue_setup(pid, 0, nb_txd,
				     rte_eth_dev_socket_id(pid), NULL);
	if (ret < 0) {
		DBG(DBG_INIT, "ctrl_bridge: tx_queue_setup(port %u) failed: %d\n", pid, ret);
		return -1;
	}

	ret = rte_eth_dev_start(pid);
	if (ret < 0) {
		DBG(DBG_INIT, "ctrl_bridge: dev_start(port %u) failed: %d\n", pid, ret);
		return -1;
	}

	/* Promiscuous so the kernel side receives whatever we push up. */
	rte_eth_promiscuous_enable(pid);

	g_ctrl_pid = pid;
	g_ready = 1;
	DBG(DBG_INIT,
	    "ctrl_bridge: ready — virtio-user port %u bridged to WAN port %u "
	    "(ctrl EtherType 0x%04x)\n", g_ctrl_pid, g_wan_pid, CB_CTRL_ETHERTYPE);
	return 0;
}

int
cb_is_ctrl_frame(struct rte_mbuf *m)
{
	if (unlikely(m->data_len < sizeof(struct rte_ether_hdr)))
		return 0;
	const struct rte_ether_hdr *eth =
		rte_pktmbuf_mtod(m, const struct rte_ether_hdr *);
	return eth->ether_type == rte_cpu_to_be_16(CB_CTRL_ETHERTYPE);
}

/*
 * A control frame arrived on the WAN port carrying CB_CTRL_ETHERTYPE. We
 * preserved the ORIGINAL EtherType (ARP or IPv4) in a 2-byte tag inserted
 * right after the Ethernet header on egress (see cb_pump_egress). Restore that
 * EtherType, remove the 2-byte tag, then TX to the virtio-user port so the
 * kernel stack on ctrl0 parses it correctly (ARP must stay ARP!).
 */
void
cb_punt_to_kernel(struct rte_mbuf *m)
{
	if (unlikely(!g_ready)) {
		rte_pktmbuf_free(m);
		return;
	}

	const uint16_t ehlen = sizeof(struct rte_ether_hdr);

	/* Need at least ETH + 2-byte original-type tag. */
	if (unlikely(m->data_len < ehlen + 2)) {
		rte_pktmbuf_free(m);
		return;
	}

	/* Read the carried original EtherType (network order) from just after
	 * the Ethernet header. */
	uint8_t *p = rte_pktmbuf_mtod(m, uint8_t *);
	uint16_t orig_type_be;
	memcpy(&orig_type_be, p + ehlen, 2);

	/* Save the Ethernet header, remove the 2-byte tag by sliding the header
	 * forward 2 bytes (adj 2 from front then rebuild ETH is messy because the
	 * tag sits AFTER the header; instead memmove the header over the tag). */
	struct rte_ether_hdr eth_copy;
	memcpy(&eth_copy, p, ehlen);
	eth_copy.ether_type = orig_type_be;     /* restore real type */

	/* Drop the 2 tag bytes: advance data start by 2, then place the saved
	 * (restored) Ethernet header at the new front. */
	if (unlikely(rte_pktmbuf_adj(m, 2) == NULL)) {
		rte_pktmbuf_free(m);
		return;
	}
	uint8_t *np = rte_pktmbuf_mtod(m, uint8_t *);
	memcpy(np, &eth_copy, ehlen);

	uint16_t sent = rte_eth_tx_burst(g_ctrl_pid, 0, &m, 1);
	if (unlikely(sent != 1)) {
		DBG(DBG_RX, "ctrl_bridge: punt TX to ctrl port dropped a frame\n");
		rte_pktmbuf_free(m);
	} else {
		DBG(DBG_RX, "ctrl_bridge: punted 1 control frame to kernel (ctrl0)\n");
	}
}

/*
 * Pull frames the kernel/keyd transmitted on ctrl0 (RX on the virtio-user
 * port). For each, PRESERVE the original EtherType (ARP/IPv4) by inserting a
 * 2-byte tag right after the Ethernet header, then set the outer EtherType to
 * CB_CTRL_ETHERTYPE so the peer's WAN classifier recognises it as control.
 * TX out the WAN port.
 *
 * Wire layout produced:
 *   [ETH dst|src|0x88B5][orig EtherType 2B][original L3 payload...]
 */
int
cb_pump_egress(void)
{
	if (unlikely(!g_ready))
		return 0;

	struct rte_mbuf *bufs[CB_BURST];
	uint16_t n = rte_eth_rx_burst(g_ctrl_pid, 0, bufs, CB_BURST);
	if (n == 0)
		return 0;

	const uint16_t ehlen = sizeof(struct rte_ether_hdr);
	uint16_t i, out = 0;
	struct rte_mbuf *txq[CB_BURST];

	for (i = 0; i < n; i++) {
		struct rte_mbuf *m = bufs[i];
		if (unlikely(m->data_len < ehlen)) {
			rte_pktmbuf_free(m);
			continue;
		}

		/* Save the Ethernet header and its original EtherType. */
		struct rte_ether_hdr eth_copy;
		memcpy(&eth_copy, rte_pktmbuf_mtod(m, void *), ehlen);
		uint16_t orig_type_be = eth_copy.ether_type;

		/* Make room for a 2-byte tag between the ETH header and the L3
		 * payload. Prepend 2 bytes at the front, then slide the Ethernet
		 * header back to the front, leaving 2 free bytes after it. */
		if (unlikely(rte_pktmbuf_prepend(m, 2) == NULL)) {
			rte_pktmbuf_free(m);
			continue;
		}
		uint8_t *p = rte_pktmbuf_mtod(m, uint8_t *);
		/* Move the ETH header to the front (over the 2 new bytes). */
		memcpy(p, &eth_copy, ehlen);
		/* Write the original EtherType tag right after the header. */
		memcpy(p + ehlen, &orig_type_be, 2);
		/* Set the outer EtherType to the control type. */
		struct rte_ether_hdr *eth = (struct rte_ether_hdr *)p;
		eth->ether_type = rte_cpu_to_be_16(CB_CTRL_ETHERTYPE);

		txq[out++] = m;
	}

	if (out == 0)
		return 0;

	uint16_t sent = rte_eth_tx_burst(g_wan_pid, 0, txq, out);
	if (unlikely(sent < out)) {
		DBG(DBG_RX, "ctrl_bridge: egress TX to WAN dropped %u frame(s)\n",
		    (unsigned)(out - sent));
		while (sent < out)
			rte_pktmbuf_free(txq[sent++]);
	}
	DBG(DBG_RX, "ctrl_bridge: pumped %u control frame(s) to WAN\n",
	    (unsigned)out);
	return out;
}

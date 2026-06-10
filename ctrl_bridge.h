/* SPDX-License-Identifier: BSD-3-Clause
 *
 * ctrl_bridge.h  --  Control-plane exception path (virtio-user) for the
 *                    DPDK encryptor.
 *
 * Purpose
 * -------
 * keyd runs in the kernel and must reach the PEER box's keyd over the single
 * DPDK-owned WAN wire (port 1). The kernel can't see port 1 (it's DPDK-bound),
 * so we give the kernel a virtual NIC (`ctrl0`) backed by a DPDK virtio-user
 * vdev. This module shuttles frames between that virtio-user port and the WAN
 * port, tagging control frames with a dedicated EtherType so they ride the
 * same wire as encrypted data without being confused for data.
 *
 * Flow:
 *   keyd → kernel → ctrl0 → [virtio-user port, DPDK sees it] → cb_pump_egress()
 *        → stamp EtherType 0x88B5 → TX out WAN port 1 → wire → peer
 *   peer → wire → WAN port 1 RX → cb_is_ctrl_frame()? → restore EtherType
 *        → TX to virtio-user port → kernel ctrl0 → keyd
 *
 * Setup (EAL arg, added to the app launch line):
 *   --vdev=virtio_user0,path=/dev/vhost-net,iface=ctrl0,queues=1
 *
 * Then on each box, AFTER the app starts (ctrl0 now exists in the kernel):
 *   ip addr add 169.254.0.1/30 dev ctrl0   # peer gets .2
 *   ip link set ctrl0 up
 *
 * This is the LEAST-tested part of the system and is expected to need
 * iteration on real hardware. All paths are traced via dbg.h (DBG_RX) so you
 * can watch control frames move when built with -DL2FWD_DEBUG.
 */

#ifndef L2FWD_CTRL_BRIDGE_H_
#define L2FWD_CTRL_BRIDGE_H_

#include <stdint.h>
#include <rte_mbuf.h>

#ifdef __cplusplus
extern "C" {
#endif

/* EtherType that marks a control (keyd handshake) frame on the WAN wire.
 * 0x88B5 is IEEE-assigned for local experimental use — it will not collide
 * with the IPv4 (0x0800) data frames the tunnel carries. Both boxes MUST use
 * the same value. */
#define CB_CTRL_ETHERTYPE 0x88B5

/* virtio-user vdev name to look for (matches the --vdev=virtio_user0 arg). */
#define CB_VDEV_NAME "virtio_user0"

/*
 * cb_init
 *   Find the virtio-user ethdev port by name and configure/start it (1 rx +
 *   1 tx queue) using the given mbuf pool. `wan_portid` is the encrypted WAN
 *   port (port 1) this bridge is paired with. Returns 0 on success, <0 if the
 *   vdev isn't present (e.g. --vdev not passed) — in which case the caller
 *   should run without the control bridge (data plane still works, but keyd
 *   has no path to the peer).
 */
int cb_init(uint16_t wan_portid, struct rte_mempool *mp);

/* Was the bridge successfully initialised? (0/1) */
int cb_ready(void);

/* The virtio-user port id (valid only if cb_ready()). */
uint16_t cb_ctrl_portid(void);

/*
 * cb_is_ctrl_frame
 *   True if this freshly-RX'd WAN frame is a control frame (carries
 *   CB_CTRL_ETHERTYPE). Cheap: one ethertype compare. Call at the very top of
 *   the WAN(port 1) RX handling, before any decrypt work.
 */
int cb_is_ctrl_frame(struct rte_mbuf *m);

/*
 * cb_punt_to_kernel
 *   Hand a control frame up to the kernel via the virtio-user port: restore
 *   its EtherType to IPv4 and TX it to the ctrl port. Takes ownership of `m`
 *   (frees on failure). Call when cb_is_ctrl_frame() returned true.
 */
void cb_punt_to_kernel(struct rte_mbuf *m);

/*
 * cb_pump_egress
 *   Drain any frames the kernel/keyd sent out ctrl0 (RX from the virtio-user
 *   port), stamp them with CB_CTRL_ETHERTYPE, and TX them out the WAN port.
 *   Call once per main-loop iteration on the WAN lcore. Returns the number of
 *   control frames forwarded (0 if none). Cheap when idle.
 */
int cb_pump_egress(void);

#ifdef __cplusplus
}
#endif

#endif /* L2FWD_CTRL_BRIDGE_H_ */

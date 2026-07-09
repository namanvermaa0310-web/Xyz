#!/bin/bash
# start_dpdk_client.sh   -- run in TERMINAL 2 on the CLIENT box.
#
# Identical DPDK invocation to the server box (both are NXP HW-crypto).
# Kills only DPDK, waits for the socket, starts DPDK in the foreground.
#
# RULE: restart key_sender (Terminal 1) => you MUST re-run this too.

set -u
SOCK=/run/l2fwd/keyd.sock

echo "=== [client] killing any old DPDK ==="
pkill -9 dpdk-l2fwd-crypto 2>/dev/null
sleep 1

echo "=== [client] waiting for key_sender socket: $SOCK ==="
while [ ! -S "$SOCK" ]; do sleep 0.2; done
echo "=== [client] socket present, starting DPDK ==="

exec ./dpdk-l2fwd-crypto -l 0-3 \
    --vdev=virtio_user0,path=/dev/vhost-net,iface=ctrl10,queues=1 \
    -n 4 -- -p 0x3 -q 1 --cdev_type HW \
    --chain CIPHER_ONLY --cipher_algo aes-cbc --no-mac-updating

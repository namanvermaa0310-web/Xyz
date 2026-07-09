#!/bin/bash
# start_dpdk_server.sh   -- run in TERMINAL 2 on the SERVER box.
#
# Kills only DPDK (NOT key_sender), waits for key_sender's socket to exist,
# then starts DPDK in the FOREGROUND so this terminal shows the stats.
#
# RULE: if you restart key_sender (Terminal 1), you MUST also re-run this
# script, because a fresh key_sender recreates the socket and DPDK would
# otherwise be stuck on the old (deleted) socket inode.

set -u
SOCK=/run/l2fwd/keyd.sock

echo "=== [server] killing any old DPDK ==="
pkill -9 dpdk-l2fwd-crypto 2>/dev/null
sleep 1

echo "=== [server] waiting for key_sender socket: $SOCK ==="
while [ ! -S "$SOCK" ]; do sleep 0.2; done
echo "=== [server] socket present, starting DPDK ==="

exec ./dpdk-l2fwd-crypto -l 0-3 \
    --vdev=virtio_user0,path=/dev/vhost-net,iface=ctrl10,queues=1 \
    -n 4 -- -p 0x3 -q 1 --cdev_type HW \
    --chain CIPHER_ONLY --cipher_algo aes-cbc --no-mac-updating

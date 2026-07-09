#!/bin/bash
# start_keysender_client.sh   -- run in TERMINAL 1 on the CLIENT box.
#
# Same cleanup as the server script, then starts key_sender_client in the
# FOREGROUND. key_sender_client is the TLS CLIENT -- it connects out to the
# server box at 192.168.10.40:4433. Start this AFTER the server box's
# key_sender is up and listening, or the first TLS handshake gets
# "Connection refused" (the built-in 5s startup delay helps but the server
# genuinely being up first is what guarantees success).

set -u
JDK=/opt/jdk-21.0.7
SOCK=/run/l2fwd/keyd.sock

echo "=== [client] cleaning old key_sender / java ==="
pkill -9 -f key_sender 2>/dev/null
pkill -9 java          2>/dev/null
sleep 1

echo "=== [client] removing stale socket ==="
rm -f "$SOCK"
mkdir -p /run/l2fwd

echo "=== [client] starting key_sender_client (foreground) ==="
echo "=== wait for: 'waiting for DPDK to connect' ==="
exec env LD_LIBRARY_PATH="$JDK/lib/server" bin/key_sender_client

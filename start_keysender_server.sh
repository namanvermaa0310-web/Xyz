#!/bin/bash
# start_keysender_server.sh   -- run in TERMINAL 1 on the SERVER box.
#
# Cleans up any old key_sender/JVM, removes the stale socket, then starts
# key_sender_server in the FOREGROUND so this terminal shows its output.
#
# Start this FIRST (before the client box's key_sender) so the TLS
# listening socket on :4433 is up before the client tries to connect.
#
# Wait until it prints "waiting for DPDK to connect" before running
# start_dpdk.sh in Terminal 2.

set -u
JDK=/opt/jdk-21.0.7
SOCK=/run/l2fwd/keyd.sock

echo "=== [server] cleaning old key_sender / java ==="
# -f matches the FULL command line, so the 15-char comm-name truncation
# (key_sender_serv) can't cause a miss.
pkill -9 -f key_sender 2>/dev/null
pkill -9 java          2>/dev/null
sleep 1

echo "=== [server] removing stale socket ==="
rm -f "$SOCK"
mkdir -p /run/l2fwd

echo "=== [server] starting key_sender_server (foreground) ==="
echo "=== wait for: 'waiting for DPDK to connect' ==="
echo "=== also confirm TLS listener: ss -tlnp | grep 4433 (in another tab) ==="
exec env LD_LIBRARY_PATH="$JDK/lib/server" bin/key_sender_server

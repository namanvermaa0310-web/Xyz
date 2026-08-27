================================================================
 Ethernet RX -> TX Loopback   (Verilog-2001, 2 files)
================================================================

  eth_loopback.v      <- THE DESIGN (this is the only file you synthesize)
  tb_eth_loopback.v   <- testbench (simulation only, not synthesized)

Run:
  iverilog -g2001 -o sim.out eth_loopback.v tb_eth_loopback.v
  ./sim.out

Result: ALL PHASES PASSED
  Phase 1  clean loopback          50 in / 50 out / 0 errors
  Phase 2  50% TX backpressure     30 in / 30 out / 0 errors
  Phase 3  all frames flagged bad  20 dropped / 0 leaked through

----------------------------------------------------------------
 WHY IT IS NOT JUST A WIRE
----------------------------------------------------------------
Conceptually yes, it is "RX in, TX out". But a direct wire does
not work, for two reasons:

1. RX has NO backpressure. TX HAS backpressure.
   RX delivers data whether you are ready or not; TX can stall
   you with tx_ready. A direct wire loses data on the first stall.

2. The MAC requires CONTIGUOUS transfer.
   tx_valid must stay asserted from SOP through EOP with no gaps.
   If TX stalls mid-packet on a direct wire, you emit a corrupt
   frame.

So the minimum that actually works is one packet FIFO doing
store-and-forward: buffer a whole frame, then send it.

----------------------------------------------------------------
 THREE BUGS THIS CODE ALREADY HAS FIXED
----------------------------------------------------------------
All three were found by the testbench during development. They
are the ones that bite people writing this from scratch:

1. Multiple-driver on the packet counter.
   A single pkt_count incremented by the RX block and decremented
   by the TX block is illegal Verilog. Fixed by using two
   counters (pkt_wr / pkt_rd) and comparing them.

2. Mid-packet FIFO overflow writing a TRUNCATED frame.
   Accept a packet at SOP when there is room, run out of room
   part way through, skip the rest - and still commit it. Silent
   corruption that only appears under load. Fixed by snapshotting
   wr_ptr at SOP and rewinding if the frame cannot complete.

3. Registered-output handshake violation.
   Using tx_ready to decide to advance while the output beat only
   appears the NEXT cycle means you can present a beat while
   tx_ready is low and move on anyway - the MAC never sees it.
   Fixed with a proper single-stage registered handshake:
   a beat is consumed only when presented AND tx_ready is high.

----------------------------------------------------------------
 ASSUMPTION
----------------------------------------------------------------
RX and TX client clocks are the SAME clock. That is normal for a
loopback (both come from the same IP instance). If they are
genuinely separate clocks you need gray-coded CDC pointers on
wr_ptr / rd_ptr - the store-and-forward structure stays the same.

----------------------------------------------------------------
 PORT NAMES
----------------------------------------------------------------
The port names here are generic. Bind them to your generated IP
wrapper - generate the F-Tile Ethernet IP in Quartus, open the
generated .v, and connect field by field. Do not assume the names
in this file match your IP version.

Also: the IP has a BUILT-IN design example generator that
produces a complete working loopback with correct reset and
initialisation sequencing. Generate and run that first.

----------------------------------------------------------------
 WHAT THIS DOES NOT VERIFY
----------------------------------------------------------------
Fabric logic only. NOT covered: PMA, PCS, RS-FEC, alignment
marker lock, link training. Simulate the real IP, then test on
hardware, before reporting that loopback works.

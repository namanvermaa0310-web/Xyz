================================================================================
 STAGE 2 - 400G LOOPBACK DATAPATH
 Agilex 7 F-Tile Ethernet Hard IP, MAC segmented client interface
================================================================================

FILES
  ftile_eth_400g_model.v   from STAGE 1
  eth400g_loopback.v       <-- STAGE 2, the design
  tb_loopback.v            testbench
  WAVEFORM_VERIFICATION_STAGE2.md

RUN
  iverilog -g2001 -o lb.out ftile_eth_400g_model.v eth400g_loopback.v tb_loopback.v
  ./lb.out

  ModelSim / Questa:
    vlog ftile_eth_400g_model.v eth400g_loopback.v tb_loopback.v
    vsim -c tb_loopback -do "run -all; quit"

================================================================================
 SETTINGS - all at the top of tb_loopback.v
================================================================================
  FRAME_LEN    64      every frame exactly this many bytes, 64..1518
  GAP_SEGS     1       idle segments between frames (0 = tight packing)
  STALL_RATE   0       0 = TX never stalls, 40 = stalls often
  INJECT_ERR   0       1 = flag ~25% of frames bad
  DROP_ON_ERR  1       1 = remove errored frames, 0 = forward them

  A beat is 128 bytes (16 segments x 8 bytes), so:
      frames per beat = 16 / (ceil(FRAME_LEN/8) + GAP_SEGS)

  The testbench PRINTS THE EXPECTED value next to the measured one, so a
  wrong result is obvious without doing the arithmetic.

================================================================================
 RESULTS
================================================================================
  len   gap stall err drop | frames/beat  dropped  ovf   mismatch viol result
  ----  --- ----- --- ---- | -----------  -------  ----  -------- ---- ------
    64   1    0    0   0   | 1.778        0        0     0        0    PASS
    64   1    0    1   1   | 1.778        8747     0     0        0    PASS
    64   0    0    0   0   | 2.000        0        0     0        0    PASS
   128   1    0    0   0   | 0.941        0        0     0        0    PASS
  1518   1    0    0   0   | 0.084        0        0     0        0    PASS
    64   1   40    0   0   | 1.778        0        1489  0        0    PASS
    64   1   40    1   1   | 1.778        8747     0     0        0    PASS

  Interface rate 424.5-424.8 Gbps of 425 (99.9-100.0%) with STALL_RATE = 0.

  8747 frames flagged bad, 8747 dropped - exact. Good frames unaffected:
  18069 of 20001 beats still pass through with zero mismatches.

================================================================================
 CHANGES FROM THE FIRST VERSION
================================================================================

 1. TX ERROR IS FULLY DECOUPLED FROM RX
    Previously o_tx_mac_error was driven from i_rx_mac_error. They mean
    different things:
      o_rx_mac_error (2b/seg)  CLASSIFIES a received frame
                               - malformed / size / payload-length
      i_tx_mac_error (1b/seg)  COMMANDS the MAC to invalidate the frame it
                               is transmitting
    Forwarding one to the other means "this arrived corrupt, so transmit it
    and mark it corrupt" - almost never what an encryptor wants.

    o_tx_mac_error now comes from a dedicated i_tx_error input.
    o_tx_mac_skip_crc likewise from i_tx_skip_crc.

 2. ERRORED FRAMES CAN BE DROPPED
    i_drop_on_error, runtime controlled.
      0 = forward errored frames untouched
      1 = remove them from the stream entirely

    Each in-frame segment is tagged with the frame it belongs to. The stream
    is held in a DROP_DELAY-beat delay line, long enough for a whole frame.
    The error arrives at EOP; by then the frame's earlier segments are still
    in the delay line, so their inframe bits are cleared retroactively before
    the beat reaches the FIFO.

 3. CDC BUG FIXED - SINGLE CLOCK
    The previous version exposed i_clk_tx and i_clk_rx separately while
    computing  level = wr_ptr - rd_ptr  with NO synchronisation. A latent
    failure: it worked only because the testbench tied both to one clock.

    Collapsed to a single clk. If the clocks ever genuinely differ, the
    pointers need gray-code CDC - better as an explicit decision than an
    accident.

 4. ERROR INJECTION IS FRACTIONAL, NOT ALL-OR-NOTHING
    Previously every frame was flagged bad, so the drop test could not show
    that GOOD frames survive. Now ~25% are errored, which is the case that
    actually matters.

 5. FRAMES-PER-BEAT USES THE RAW ARRIVAL COUNT
    o_rx_beats counts beats the DUT ACCEPTED, which is post-drop. Using it
    as the denominator made dropped beats vanish and inflated the ratio to
    1.968. The testbench now counts raw RX arrivals separately.

================================================================================
 THE THREE DESIGN DECISIONS
================================================================================

 1. NO PACKET LAYER
    UG sec 7.5: a packet may start and the previous one end in the same
    cycle. UG sec 7.4 mandates tight packing. At 128 bytes per beat against
    64-byte minimum frames, two frames per beat is routine - stage 1 measured
    exactly 2.000.

    The datapath buffers and replays beats verbatim, carrying inframe /
    eop_empty / error / skip_crc as opaque sideband. Multi-frame-per-beat is
    not "handled" - it is structurally impossible to get wrong.

    (The drop stage is the one exception: it must know frame boundaries. See
    the assumption below.)

 2. TX IS A FIXED-LATENCY PAUSE INTERFACE
    UG sec 7.4: valid asserts whenever ready is asserted "even though there
    is no packet to send", spaced by a fixed 1-7 cycle latency, and the bus
    freezes while valid is low.

    So o_tx_mac_valid is i_tx_mac_ready DELAYED, never derived from having
    data. An idle beat is inframe=0, NOT valid=0.

    "out_free = ~valid | ready" is the WRONG protocol. It compiles, passes a
    naive testbench, and fails on silicon.

 3. LATENCY-AGNOSTIC BY CONSTRUCTION
    Stage 3 will hold a custom algorithm whose latency is not under our
    control and may change. Nothing may depend on knowing it:

      - initiation interval = 1, no internal stall path
      - latency is a PARAMETER, never a constant
      - every register gated by the same enable
      - the algorithm block has NO backpressure output

    Consequence: throughput is independent of frame size. Latency costs
    buffering and delay, never throughput.

    Why this matters at 400G specifically: hiding crypto latency in idle
    cycles works at 100G only because a 512-bit datapath at 390.625 MHz
    gives 200 Gb/s for 100 Gb/s of traffic - 50% idle. At 400G the interface
    runs 425 Gb/s for 400 Gb/s of payload: about 6% idle. Hiding even 17
    cycles that way would need a ~35 KB frame. Not a tuning problem - it is
    arithmetically impossible at any legal frame size.

================================================================================
 >>> ASSUMPTION - VERIFY BEFORE HARDWARE <<<
================================================================================
 The DROP feature needs frame boundaries, and resolves them from inframe
 1->0 transitions.

 UG sec 7.4 mandates tight packing with no idle segments, under which inframe
 would stay high across a frame boundary and leave no transition to find. But
 that mandate is about what YOU DRIVE ON TX for maximum throughput.

 On RX the IP delivers what arrived on the wire, and Ethernet always carries
 an inter-packet gap (minimum 8 octets = 1 segment), so RX should always show
 at least one idle segment between frames.

 THIS IS AN INFERENCE FROM THE IPG REQUIREMENT, NOT A QUOTATION.
 Confirm against the generated design example.

 Practical consequence: run with GAP_SEGS >= 1 when drop is enabled. If RX
 really does pack with zero gap, only DROP is affected - the rest of the
 datapath is frame-agnostic.

================================================================================
 SIZING NOTES
================================================================================
 DROP_DELAY must exceed the longest frame's span in beats:
     beats per frame = ceil( (ceil(LEN/8) + GAP_SEGS) / 16 )
       1518 B -> 12 beats  -> DROP_DELAY 16 is enough
       9000 B -> 71 beats  -> JUMBO NEEDS DROP_DELAY 80
 Cost ~1184 flip-flops per stage at this datapath width.

 FIFO uses a REGISTERED read with an explicit M20K ramstyle. An asynchronous
 read infers MLAB: ~896 MLABs at 1120 bits x 512 deep, built from ALMs, and
 it will not close 415 MHz. Registered read infers ~28 M20Ks.

================================================================================
 PHASE WITH STALLS IS NOT A FAILURE
================================================================================
 1489 dropped beats under sustained TX stalling is EXPECTED. RX takes no
 backpressure (UG sec 7.5) and there is no flow-control path back to the
 source, so a full FIFO must drop.

 What matters is mismatch = 0 alongside it: every beat that gets through is
 bit-exact. Drops without corruption.

 The production answer is PAUSE/PFC flow control (UG sec 4.2.3).

================================================================================
 NOT PROVEN BY STAGE 2
================================================================================
 Reset and status sequencing (12-step, o_tx_lanes_stable / o_rx_pcs_ready)
 Any processing stage - that is STAGE 3
 PMA, PCS, RS-FEC (KP4), lane distribution, AM lock, AN/LT, link training
 PAUSE/PFC flow control, skip_crc semantics, CSR/statistics, PTP/TOD

================================================================================
 NEXT
================================================================================
 STAGE 3  pipe_proc - pipelined processing, II=1, latency a parameter,
          frozen by the same enable. Placeholder for the custom algorithm.
 STAGE 4  top wrapper for synthesis and fitting on Agilex 7.

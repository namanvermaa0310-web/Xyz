================================================================================
 STAGE 2 - 400G LOOPBACK DATAPATH
================================================================================

FILES
  ftile_eth_400g_model.v   from STAGE 1, unchanged
  eth400g_loopback.v       <-- STAGE 2, the design under test
  tb_loopback.v            testbench
  WAVEFORM_VERIFICATION_STAGE2.md

SETTING THE FRAME SIZE
  Open tb_loopback.v. Near the top there is ONE place to change:

      localparam FRAME_LEN  = 64;      // <-- CHANGE THIS
      localparam STALL_RATE = 0;       // 0 = never stall, 40 = stalls often
      localparam RUN_CYCLES = 20000;
      localparam INJECT_ERR = 0;       // 1 = flag every frame as bad

  EVERY frame in the run will be exactly FRAME_LEN bytes. No mixed sizes.

  A beat is 128 bytes (16 segments x 8 bytes), so:

      frames per beat = 128 / FRAME_LEN

  FRAME_LEN   segments/frame   frames/beat   eop_empty
  ---------   --------------   -----------   ---------
      64             8            2.000       0   (multiple of 8)
     128            16            1.000       0
     256            32            0.500       0
    1400           175            0.091       0
    1401           176            0.091       7   (not a multiple of 8)
    1518           190            0.084       2

  eop_empty is non-zero ONLY when FRAME_LEN is not a multiple of 8. That is
  correct behaviour, not a bug - a frame ending exactly on a segment boundary
  has zero empty bytes.

  The testbench PRINTS THE EXPECTED frames-per-beat next to the measured one,
  so a wrong result is obvious without doing the arithmetic yourself.

RUN
  iverilog -g2001 -o lb.out ftile_eth_400g_model.v eth400g_loopback.v tb_loopback.v
  ./lb.out

  ModelSim / Questa:
    vlog ftile_eth_400g_model.v eth400g_loopback.v tb_loopback.v
    vsim -c tb_loopback -do "run -all; quit"

RESULT  -  frame size sweep, STALL_RATE = 0

  FRAME_LEN  frames/beat (expected)  rate              mismatch  result
      64        2.000  (2.000)       424.8 (100.0%)    0         PASS
     128        1.000  (1.000)       424.8 (100.0%)    0         PASS
     256        0.500  (0.500)       424.8 (100.0%)    0         PASS
    1400        0.091  (0.091)       424.8 (100.0%)    0         PASS
    1518        0.084  (0.084)       424.8 (100.0%)    0         PASS

  With STALL_RATE = 40 the rate drops to ~382 Gbps (90%) and the FIFO
  overflows by ~1490 beats - both expected, see the note below. Data
  mismatches stay at 0.

--------------------------------------------------------------------------------
 WHAT STAGE 2 ADDS OVER STAGE 1
--------------------------------------------------------------------------------
  Stage 1 had no datapath, so there was nothing to corrupt. Stage 2 introduces
  one, which makes three new things testable:

    1. DATA INTEGRITY - beat-exact comparison, 0 mismatches in all phases
    2. TX PAUSE PROTOCOL UNDER LOAD - stage 1 only exercised it on idle beats
    3. THROUGHPUT - 424.8 of 425 Gbps, 100.0% of interface capacity

--------------------------------------------------------------------------------
 THE THREE DESIGN DECISIONS
--------------------------------------------------------------------------------
 1. NO PACKET LAYER
    Stage 1 measured exactly 2.000 frames per beat at 64-byte frame size.
    UG sec 7.5 confirms a packet may start and the previous one end in the
    same cycle, and sec 7.4 mandates tight packing.

    So this module never inspects frame boundaries. It buffers and replays
    1024-bit beats verbatim, carrying inframe / eop_empty / error / skip_crc
    as opaque sideband. Multi-frame-per-beat is not "handled" - it is
    structurally impossible to get wrong.

    Phase B confirms 40000 frames through 20000 beats with 0 mismatches.

 2. TX IS A FIXED-LATENCY PAUSE INTERFACE
    UG sec 7.4: valid asserts whenever ready is asserted "even though there
    is no packet to send", spaced by a fixed 1-7 cycle latency, and the bus
    must freeze while valid is low.

    So o_tx_mac_valid is i_tx_mac_ready DELAYED, never derived from having
    data. An idle beat is inframe=0, NOT valid=0.

    A conventional "out_free = ~valid | ready" handshake is the WRONG
    protocol. It compiles, passes a naive testbench, and fails on silicon.

 3. REGISTERED FIFO READ
    An asynchronous read makes Quartus infer MLAB. At 1120 bits x 512 deep
    that is ~896 MLABs, built from ALMs - it will not close 415 MHz.
    A registered read infers M20K: ~28 blocks. Cost is one cycle of latency.

--------------------------------------------------------------------------------
 PHASE D IS NOT A FAILURE
--------------------------------------------------------------------------------
  1490 dropped beats under sustained TX stalling is EXPECTED. RX takes no
  backpressure (UG sec 7.5) and there is no flow-control path back to the
  source, so a full FIFO must drop.

  What matters is that mismatch stays at 0: every beat that does get through
  is bit-exact. Drops without corruption.

  The production answer is PAUSE/PFC flow control (UG sec 4.2.3), which stops
  the far end rather than dropping. That belongs in a later stage.

--------------------------------------------------------------------------------
 NOT PROVEN BY STAGE 2
--------------------------------------------------------------------------------
  Reset and status sequencing (12-step, o_tx_lanes_stable / o_rx_pcs_ready)
  Any processing stage - that is STAGE 3 (pipe_proc)
  PMA, PCS, RS-FEC (KP4), lane distribution, AM lock, AN/LT, link training
  PAUSE/PFC flow control, skip_crc semantics, CSR/statistics, PTP/TOD

--------------------------------------------------------------------------------
 NEXT
--------------------------------------------------------------------------------
  STAGE 3  add pipe_proc - the pipelined processing stage, II=1, frozen by
           the same tx_en. Placeholder for AES-GCM / SecY.
  STAGE 4  top-level wrapper for synthesis and fitting on Agilex 7.

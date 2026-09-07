//=============================================================================
// eth400g_loopback.v     -- Verilog-2001
//
//  STAGE 2 : 400G LOOPBACK DATAPATH
//
//  RX MAC segmented  ->  elastic beat FIFO  ->  TX MAC segmented
//
//  No processing stage. That is STAGE 3 (pipe_proc).
//
//  Conforms to F-Tile Ethernet Hard IP User Guide, doc 683023, sec 7.4 / 7.5.
//
//=============================================================================
// DESIGN POINT 1 - THERE IS NO PACKET LAYER
//=============================================================================
// UG sec 7.5:
//   "Packets may start on any 8-byte segment... For multisegmented
//    interfaces, a new packet may start and the previous packet end are
//    within the same cycle."
//
// UG sec 7.4 (Attention):
//   "To achieve the maximum throughput when using the TX MAC segmented
//    interface, the input packets need to be packed tightly, leaving no idle
//    segments in between."
//
// A beat is 1024 bits = 128 bytes. The minimum Ethernet frame is 64 bytes.
// So TWO complete frames arrive in ONE clock cycle as a matter of routine,
// not as a corner case. Stage 1 demonstrated exactly 2.000 frames per beat.
//
// Any design holding ONE packet state per beat - a single sop pointer, a
// single in_packet flag, "count a packet on any_eop" - miscounts and
// eventually commits truncated frames. It passes light testing and corrupts
// traffic under load.
//
// This module therefore has NO packet layer. It buffers and replays 1024-bit
// beats verbatim, carrying inframe / eop_empty / error / skip_crc through as
// opaque sideband. Frame boundaries are never inspected, so multi-frame-per-
// beat is not "handled" - it is structurally impossible to get wrong.
//
// Handling it with packet bookkeeping would need 16 parallel packet state
// machines. For a loopback that buys nothing: what goes out is exactly what
// came in.
//
//=============================================================================
// DESIGN POINT 2 - TX IS A FIXED-LATENCY PAUSE INTERFACE, NOT READY/VALID
//=============================================================================
// UG sec 7.4, verbatim:
//   "The i_tx_mac_valid signal deasserts when the o_tx_mac_ready signal is
//    deasserted. The i_tx_mac_valid signal asserts only when the
//    o_tx_mac_ready signal is asserted, even though there is no packet to
//    send."
//   "The i_tx_mac_valid and the o_tx_mac_ready signals can be spaced by a
//    fixed latency between 1 to 7 clock cycles."
//   "When i_tx_mac_valid deasserts, i_tx_mac_data, i_tx_mac_inframe,
//    i_tx_mac_eop_empty, i_tx_mac_error and i_tx_skip_crc signals must be
//    paused for as many cycles as o_tx_mac_ready is deasserted."
//
// Consequences:
//   * o_tx_mac_valid is i_tx_mac_ready DELAYED by READY_LATENCY.
//     It is NOT derived from whether we have data.
//   * With nothing to send we STILL assert valid and drive inframe = 0.
//     An IDLE BEAT IS inframe=0, NOT valid=0.
//   * While ready is low the entire output bus freezes.
//
// A classic handshake ("out_free = ~valid | ready") is the WRONG protocol
// here. It compiles, it passes a naive testbench, and it fails on silicon.
//
//=============================================================================
// DESIGN POINT 3 - LATENCY-AGNOSTIC BY CONSTRUCTION
//=============================================================================
// The processing block added in STAGE 3 will hold a custom algorithm whose
// latency is NOT under our control and may change. Nothing in this datapath
// may depend on knowing it.
//
// Rules every stage must satisfy:
//   1. INITIATION INTERVAL = 1. One beat in, one beat out, every enabled
//      cycle. No internal stall path.
//   2. LATENCY IS A PARAMETER, NEVER A CONSTANT. No logic reads a pipeline
//      depth to make a decision. Change the parameter, nothing else moves.
//   3. EVERY REGISTER GATED BY THE SAME ENABLE. When the MAC pauses the whole
//      pipeline freezes together. A stage that keeps advancing while another
//      holds loses data - and only under stall, which is the worst kind of
//      bug to find.
//   4. THE ALGORITHM BLOCK HAS NO BACKPRESSURE OUTPUT. No ready, no busy, no
//      stall. If it can say "not now", downstream logic starts depending on
//      its timing again.
//
// Consequence: THROUGHPUT IS INDEPENDENT OF FRAME SIZE. Latency costs
// buffering and end-to-end delay, never throughput. At 1024 bits per beat,
// 40 cycles of latency needs ~5 KB of storage - a handful of M20Ks.
//
// This matters at 400G specifically. A design that hides crypto latency in
// idle cycles works at 100G only because a 512-bit datapath at 390.625 MHz
// gives 200 Gb/s for 100 Gb/s of traffic - 50% idle. At 400G the interface
// runs 425 Gb/s for 400 Gb/s of payload: about 6% idle. Hiding even 17 cycles
// of latency that way would need a ~35 KB frame. It is not a tuning problem,
// it is arithmetically impossible at any legal frame size.
//
//=============================================================================
// DESIGN POINT 4 - REGISTERED FIFO READ (fitting)
//=============================================================================
// An asynchronous read  ( wire dout = mem[rd_ptr]; )  makes Quartus infer
// MLAB / distributed LUTRAM. At 1120 bits x 1024 deep that is ~1792 MLABs,
// built from ALMs - enormous logic, and it will not close 415 MHz.
//
// A REGISTERED read infers M20K: ~56 blocks, trivial on an 027 device.
// Cost is one cycle of read latency, absorbed by the output stage.
//=============================================================================
`timescale 1ps/1ps

module eth400g_loopback #(
    parameter DATA_W        = 1024,
    parameter NUM_SEG       = 16,
    parameter EMPTY_W       = 3,
    parameter READY_LATENCY = 3,     // MUST match the IP configuration, 1..7
    parameter FIFO_DEPTH    = 512,
    parameter ADDR_W        = 9,
    // DROP_DELAY must exceed the longest frame's span in beats, otherwise a
    // frame's first beats leave the delay line before its EOP error arrives
    // and cannot be retracted.
    //
    //   beats per frame = ceil( (ceil(LEN/8) + GAP_SEGS) / 16 )
    //     1518 B  -> (190 + 1) / 16 = 12 beats   -> 16 is enough
    //     9000 B  -> (1125 + 1) / 16 = 71 beats  -> JUMBO NEEDS DROP_DELAY 80
    //
    // Cost is ~1184 flip-flops per stage at this datapath width.
    parameter DROP_DELAY    = 16,
    parameter TAG_W         = 6      // frame tags; 2**TAG_W > frames in window
)(
    // SINGLE CLOCK.
    //
    // The previous version exposed i_clk_tx and i_clk_rx separately while
    // computing  level = wr_ptr - rd_ptr  with NO synchronisation between the
    // two domains. That is a latent CDC failure: it only worked because the
    // testbench tied both ports to one clock.
    //
    // If TX and RX clocks are ever genuinely independent, the pointers need
    // gray-code CDC. Until that is required, one clock is the honest port.
    input  wire                        clk,
    input  wire                        rst_n,

    // ---- RX MAC segmented client (from IP). Takes NO backpressure. ----
    input  wire [DATA_W-1:0]           i_rx_mac_data,
    input  wire                        i_rx_mac_valid,
    input  wire [NUM_SEG-1:0]          i_rx_mac_inframe,
    input  wire [NUM_SEG*EMPTY_W-1:0]  i_rx_mac_eop_empty,
    input  wire [NUM_SEG-1:0]          i_rx_mac_fcs_error,
    input  wire [NUM_SEG*2-1:0]        i_rx_mac_error,     // 2 bits/segment

    //=====================================================================
    // TX SIDEBAND - INDEPENDENT OF RX
    //
    // o_tx_mac_error is NOT derived from i_rx_mac_error. They mean different
    // things:
    //   o_rx_mac_error (2b/seg) CLASSIFIES a received frame
    //                           - malformed / size / payload-length
    //   i_tx_mac_error (1b/seg) COMMANDS the MAC to invalidate the frame
    //                           it is transmitting
    // Forwarding one to the other means "this arrived bad, so transmit it
    // and mark it bad" - almost never what an encryptor wants.
    //
    // Drive these from your own logic. Tie to 0 for a plain loopback.
    //=====================================================================
    input  wire [NUM_SEG-1:0]          i_tx_error,
    input  wire [NUM_SEG-1:0]          i_tx_skip_crc,

    //=====================================================================
    // DROP CONTROL
    //   0 = forward errored frames, sideband untouched (MARK behaviour)
    //   1 = remove errored frames from the stream entirely
    //=====================================================================
    input  wire                        i_drop_on_error,

    // ---- TX MAC segmented client (to IP) ----
    output reg  [DATA_W-1:0]           o_tx_mac_data,
    output reg                         o_tx_mac_valid,
    output reg  [NUM_SEG-1:0]          o_tx_mac_inframe,
    output reg  [NUM_SEG*EMPTY_W-1:0]  o_tx_mac_eop_empty,
    output reg  [NUM_SEG-1:0]          o_tx_mac_error,     // 1 bit/segment
    output reg  [NUM_SEG-1:0]          o_tx_mac_skip_crc,
    input  wire                        i_tx_mac_ready,

    // ---- Status / observability ----
    output reg  [31:0]                 o_rx_beats,
    output reg  [31:0]                 o_tx_beats,
    output reg  [31:0]                 o_ovf_beats,
    output reg  [31:0]                 o_dropped_frames,
    output wire [ADDR_W:0]             o_fifo_level
);

    // Sideband carried through the FIFO alongside the data
    localparam SB_W    = NUM_SEG + NUM_SEG*EMPTY_W + NUM_SEG + NUM_SEG;
    localparam ENTRY_W = DATA_W + SB_W;

    //=====================================================================
    // FRAME TRACKING AND ERROR DROP
    //
    // ASSUMPTION - VERIFY BEFORE HARDWARE
    //   UG sec 7.4 mandates tight packing with no idle segments, which would
    //   make inframe stay high across a frame boundary and leave no 1->0
    //   transition to mark EOP. But that mandate is about what YOU DRIVE ON
    //   TX for maximum throughput.
    //
    //   On RX the IP delivers what arrived on the wire, and Ethernet always
    //   carries an inter-packet gap (minimum 8 octets = 1 segment). So RX
    //   should always show at least one idle segment between frames, making
    //   an inframe 1->0 transition a reliable EOP marker HERE.
    //
    //   This is an INFERENCE from the IPG requirement, not a quotation.
    //   Confirm against the generated design example. If RX really does pack
    //   with zero gap, frame-boundary tracking needs another mechanism and
    //   only DROP is affected - the rest of the datapath is frame-agnostic.
    //
    // HOW THE DROP WORKS
    //   Every in-frame segment is tagged with the frame it belongs to. The
    //   stream is held in a DROP_DELAY-beat delay line, long enough for a
    //   whole frame. An error is reported at EOP; by then the frame's earlier
    //   segments are still in the delay line, so their inframe bits can be
    //   cleared retroactively before the beat reaches the FIFO.
    //=====================================================================
    localparam NTAG = 1 << TAG_W;

    reg [TAG_W-1:0] cur_tag;      // tag of the frame currently open
    reg [TAG_W-1:0] next_tag;     // next tag to allocate
    reg             frame_open;
    reg [NTAG-1:0]  drop_set;     // bit per tag: this frame is bad

    // Per-segment tag for the beat being tagged this cycle
    reg [NUM_SEG*TAG_W-1:0] seg_tag;

    integer         t;
    reg [TAG_W-1:0] v_cur, v_next;
    reg             v_open;
    reg [NTAG-1:0]  v_drop;
    reg             seg_bad;
    integer         v_ndrop;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cur_tag  <= 0;
            next_tag <= 0;
            frame_open <= 1'b0;
            drop_set <= {NTAG{1'b0}};
            seg_tag  <= 0;
            o_dropped_frames <= 0;
        end else if (i_rx_mac_valid) begin

            v_cur  = cur_tag;
            v_next = next_tag;
            v_open = frame_open;
            v_drop  = drop_set;
            v_ndrop = 0;
            seg_tag = 0;

            for (t = 0; t < NUM_SEG; t = t + 1) begin
                if (i_rx_mac_inframe[t]) begin
                    if (!v_open) begin
                        // inframe 0 -> 1 : start of a new frame
                        v_cur          = v_next;
                        v_next         = v_next + 1'b1;
                        v_drop[v_cur]  = 1'b0;      // reuse: clear stale flag
                        v_open         = 1'b1;
                    end
                    seg_tag[t*TAG_W +: TAG_W] = v_cur;

                    // Error / FCS bits are valid on EOP segments (UG Table 46)
                    seg_bad = (|i_rx_mac_error[t*2 +: 2]) | i_rx_mac_fcs_error[t];
                    if (seg_bad && !v_drop[v_cur]) begin
                        v_drop[v_cur] = 1'b1;
                        v_ndrop       = v_ndrop + 1;   // first time only
                    end
                end else begin
                    // inframe 1 -> 0 : the frame closed on the previous segment
                    v_open = 1'b0;
                end
            end

            cur_tag    <= v_cur;
            next_tag   <= v_next;
            frame_open <= v_open;
            drop_set   <= v_drop;
            if (i_drop_on_error)
                o_dropped_frames <= o_dropped_frames + v_ndrop;
        end
    end

    //---------------------------------------------------------------------
    // Delay line: hold the stream long enough for the EOP error to arrive
    //---------------------------------------------------------------------
    reg [DATA_W-1:0]          dl_data    [0:DROP_DELAY-1];
    reg [NUM_SEG-1:0]         dl_inframe [0:DROP_DELAY-1];
    reg [NUM_SEG*EMPTY_W-1:0] dl_empty   [0:DROP_DELAY-1];
    reg [NUM_SEG*TAG_W-1:0]   dl_tag     [0:DROP_DELAY-1];
    reg                       dl_valid   [0:DROP_DELAY-1];

    integer d;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (d = 0; d < DROP_DELAY; d = d + 1) begin
                dl_data[d]    <= 0;
                dl_inframe[d] <= 0;
                dl_empty[d]   <= 0;
                dl_tag[d]     <= 0;
                dl_valid[d]   <= 1'b0;
            end
        end else begin
            dl_data[0]    <= i_rx_mac_data;
            dl_inframe[0] <= i_rx_mac_inframe;
            dl_empty[0]   <= i_rx_mac_eop_empty;
            dl_tag[0]     <= seg_tag;
            dl_valid[0]   <= i_rx_mac_valid;
            for (d = 1; d < DROP_DELAY; d = d + 1) begin
                dl_data[d]    <= dl_data[d-1];
                dl_inframe[d] <= dl_inframe[d-1];
                dl_empty[d]   <= dl_empty[d-1];
                dl_tag[d]     <= dl_tag[d-1];
                dl_valid[d]   <= dl_valid[d-1];
            end
        end
    end

    //---------------------------------------------------------------------
    // Mask out segments belonging to frames marked bad
    //---------------------------------------------------------------------
    wire [DATA_W-1:0]          q_data    = dl_data[DROP_DELAY-1];
    wire [NUM_SEG-1:0]         q_inframe = dl_inframe[DROP_DELAY-1];
    wire [NUM_SEG*EMPTY_W-1:0] q_empty   = dl_empty[DROP_DELAY-1];
    wire [NUM_SEG*TAG_W-1:0]   q_tag     = dl_tag[DROP_DELAY-1];
    wire                       q_valid   = dl_valid[DROP_DELAY-1];

    wire [NUM_SEG-1:0] keep;
    genvar gk;
    generate
      for (gk = 0; gk < NUM_SEG; gk = gk + 1) begin : g_keep
        assign keep[gk] = q_inframe[gk] &
                          ~(i_drop_on_error &
                            drop_set[q_tag[gk*TAG_W +: TAG_W]]);
      end
    endgenerate

    //=====================================================================
    // ELASTIC BEAT FIFO
    //
    // Simple dual-port: write on the RX clock, read on the TX clock.
    // Explicit ramstyle keeps it in M20K, not MLAB.
    //=====================================================================
    (* ramstyle = "M20K, no_rw_check" *)
    reg [ENTRY_W-1:0] mem [0:FIFO_DEPTH-1];

    reg [ADDR_W:0] wr_ptr, rd_ptr;

    wire [ADDR_W:0] level = wr_ptr - rd_ptr;
    wire            full  = (level >= FIFO_DEPTH);
    wire            empty = (wr_ptr == rd_ptr);
    assign o_fifo_level = level;

    // TX sideband comes from the DEDICATED INPUTS, not from RX.
    wire [ENTRY_W-1:0] wr_entry = { q_data,
                                    keep,          // masked inframe
                                    q_empty,
                                    i_tx_error,    // independent
                                    i_tx_skip_crc };

    //---------------------------------------------------------------------
    // WRITE SIDE
    //
    // i_rx_mac_valid qualifies the whole interface cycle.
    // i_rx_mac_inframe says which segments carry frame data.
    // These mean DIFFERENT things - both are required. A beat with valid=1
    // but inframe=0 carries no frame content and must not occupy the FIFO.
    //---------------------------------------------------------------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wr_ptr      <= 0;
            o_rx_beats  <= 0;
            o_ovf_beats <= 0;
        end else if (q_valid && (|keep)) begin
            o_rx_beats <= o_rx_beats + 1'b1;
            if (!full) begin
                mem[wr_ptr[ADDR_W-1:0]] <= wr_entry;
                wr_ptr <= wr_ptr + 1'b1;
            end else begin
                // UG sec 7.5: "The interface does not take direct
                // backpressure." RX cannot be stalled, so a full FIFO means
                // the beat is lost. Counted here so it can never be silent.
                //
                // The production answer is PAUSE/PFC flow control (UG
                // sec 4.2.3), which stops the far end rather than dropping.
                o_ovf_beats <= o_ovf_beats + 1'b1;
            end
        end
    end

    //=====================================================================
    // TX PAUSE CONTROL - the heart of the protocol
    //=====================================================================
    reg [7:0] rdy_pipe;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) rdy_pipe <= 8'd0;
        else        rdy_pipe <= {rdy_pipe[6:0], i_tx_mac_ready};
    end

    // The outputs below are REGISTERED, which adds one cycle. Tapping at
    // [READY_LATENCY-2] makes o_tx_mac_valid land exactly READY_LATENCY
    // cycles after i_tx_mac_ready. Tapping at [READY_LATENCY-1] is off by
    // one - a real bug caught by the model's protocol checker.
    wire tx_en = (READY_LATENCY >= 2) ? rdy_pipe[READY_LATENCY-2]
                                       : i_tx_mac_ready;

    //=====================================================================
    // TX OUTPUT with registered FIFO read
    //=====================================================================
    reg [ENTRY_W-1:0] mem_q;      // registered read data
    reg               rd_vld_d;   // a read was issued last cycle

    always @(posedge clk) begin
        if (tx_en) mem_q <= mem[rd_ptr[ADDR_W-1:0]];
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rd_ptr             <= 0;
            rd_vld_d           <= 1'b0;
            o_tx_mac_data      <= 0;
            o_tx_mac_valid     <= 1'b0;
            o_tx_mac_inframe   <= 0;
            o_tx_mac_eop_empty <= 0;
            o_tx_mac_error     <= 0;
            o_tx_mac_skip_crc  <= 0;
            o_tx_beats         <= 0;
        end else begin

            // valid ALWAYS tracks ready delayed by READY_LATENCY. Never
            // gated on having data. UG sec 7.4: valid asserts whenever ready
            // is asserted "even though there is no packet to send".
            o_tx_mac_valid <= tx_en;

            if (tx_en) begin
                // Issue the next read one cycle ahead
                if (!empty) begin
                    rd_ptr   <= rd_ptr + 1'b1;
                    rd_vld_d <= 1'b1;
                end else begin
                    rd_vld_d <= 1'b0;
                end

                // Drive the beat fetched last cycle
                if (rd_vld_d) begin
                    {o_tx_mac_data, o_tx_mac_inframe, o_tx_mac_eop_empty,
                     o_tx_mac_error, o_tx_mac_skip_crc} <= mem_q;
                    o_tx_beats <= o_tx_beats + 1'b1;
                end else begin
                    // IDLE BEAT: inframe = 0 with valid still asserted.
                    // Deasserting valid here would break the protocol.
                    o_tx_mac_data      <= 0;
                    o_tx_mac_inframe   <= 0;
                    o_tx_mac_eop_empty <= 0;
                    o_tx_mac_error     <= 0;
                    o_tx_mac_skip_crc  <= 0;
                end
            end
            // tx_en low: every output register holds. The whole bus is
            // frozen, exactly as UG sec 7.4 requires.
        end
    end

endmodule

//=============================================================================
// tb_iface_only.v     -- Verilog-2001
//
//  STAGE 1 : 400GE MAC SEGMENTED CLIENT INTERFACE  --  SMOKE TEST
//
//  Instantiates ftile_eth_400g_model ONLY.
//  No loopback. No FIFO. No pipeline. No processing.
//
//  PURPOSE
//    Prove the interface itself behaves correctly, in isolation, before any
//    datapath is attached. A fault found here is trivial to diagnose; the
//    same fault found inside a full loopback looks like a FIFO problem.
//
//    (Not hypothetical. Running this standalone immediately exposed a frame
//     counter bug in the model: a non-blocking increment executed twice in
//     one always block collapses to a SINGLE increment, so 64-byte frames
//     were counted at half rate. Inside the loopback that would have looked
//     like dropped packets.)
//
//  REFERENCE
//    F-Tile Ethernet Hard IP User Guide, doc 683023
//      sec 5    Clocks  - MAC datapath clock 415.0390625 MHz
//      sec 7.4  TX MAC Segmented Client Interface  (Table 43)
//      sec 7.5  RX MAC Segmented Client Interface  (Table 46)
//
//  RUN
//    iverilog -g2001 -o iface.out ftile_eth_400g_model.v tb_iface_only.v
//    ./iface.out
//
//    ModelSim / Questa:
//      vlog ftile_eth_400g_model.v tb_iface_only.v
//      vsim -c tb_iface_only -do "run -all; quit"
//=============================================================================
`timescale 1ps/1ps

module tb_iface_only;

    //=====================================================================
    // 400GE-8 geometry (UG Table 43 / Table 46)
    //=====================================================================
    localparam DATA_W  = 1024;   // client datapath width
    localparam NUM_SEG = 16;     // segments per beat
    localparam SEG_B   = 8;      // bytes per segment
    localparam EMPTY_W = 3;      // eop_empty bits per segment
    localparam BEAT_B  = NUM_SEG * SEG_B;   // 128 bytes per beat
    localparam RDY_LAT = 3;      // TX ready latency, legal range 1..7

    // MAC datapath clock = 415.0390625 MHz.
    //   UG sec 5: i_clk_tx / i_clk_rx are driven by o_clk_pll, which is
    //   "415.0390625 MHz or higher for all Ethernet modes with IEEE 802.3
    //    RS(544,514) (CL134)"  -- RS(544,514) is KP4 FEC, i.e. 400GE.
    //   Half period = 1 / (2 x 415.0390625 MHz) = 1204.75 ps
    //
    //   NOT 390.625 MHz. That is o_clk_tx_div (TX SERDES rate / 68), used
    //   for TOD/PTP - a different clock entirely.
    localparam CLK_HALF = 1204.75;

    //=====================================================================
    reg clk   = 1'b0;
    reg rst_n = 1'b0;
    always #(CLK_HALF) clk = ~clk;

    reg        gen_enable   = 1'b0;
    reg [15:0] force_len    = 16'd0;
    reg        inject_error = 1'b0;
    reg [7:0]  stall_rate   = 8'd0;

    // ---- RX MAC segmented client (model -> here) ----
    wire [DATA_W-1:0]          rx_data;
    wire                       rx_valid;
    wire [NUM_SEG-1:0]         rx_inframe;
    wire [NUM_SEG*EMPTY_W-1:0] rx_eop_empty;
    wire [NUM_SEG-1:0]         rx_fcs_error;
    wire [NUM_SEG*2-1:0]       rx_error;      // 2 bits per segment
    wire [NUM_SEG*3-1:0]       rx_status;     // 3 bits per segment

    // ---- TX MAC segmented client (stub -> model) ----
    reg  [DATA_W-1:0]          tx_data;
    reg                        tx_valid;
    reg  [NUM_SEG-1:0]         tx_inframe;
    reg  [NUM_SEG*EMPTY_W-1:0] tx_eop_empty;
    reg  [NUM_SEG-1:0]         tx_error;
    reg  [NUM_SEG-1:0]         tx_skip_crc;
    wire                       tx_ready;

    wire [31:0] m_rx_frames, m_tx_frames, m_viol;

    //=====================================================================
    ftile_eth_400g_model #(
        .DATA_W(DATA_W), .NUM_SEG(NUM_SEG), .EMPTY_W(EMPTY_W),
        .READY_LATENCY(RDY_LAT), .MIN_LEN(64), .MAX_LEN(1518)
    ) u_ip (
        .i_clk_tx(clk), .i_clk_rx(clk), .rst_n(rst_n),
        .i_gen_enable(gen_enable),
        .i_force_len(force_len),
        .i_inject_error(inject_error),
        .i_tx_stall_rate(stall_rate),
        .o_rx_mac_data(rx_data),
        .o_rx_mac_valid(rx_valid),
        .o_rx_mac_inframe(rx_inframe),
        .o_rx_mac_eop_empty(rx_eop_empty),
        .o_rx_mac_fcs_error(rx_fcs_error),
        .o_rx_mac_error(rx_error),
        .o_rx_mac_status_data(rx_status),
        .i_tx_mac_data(tx_data),
        .i_tx_mac_valid(tx_valid),
        .i_tx_mac_inframe(tx_inframe),
        .i_tx_mac_eop_empty(tx_eop_empty),
        .i_tx_mac_error(tx_error),
        .i_tx_mac_skip_crc(tx_skip_crc),
        .o_tx_mac_ready(tx_ready),
        .o_model_rx_frames(m_rx_frames),
        .o_model_tx_frames(m_tx_frames),
        .o_model_proto_viol(m_viol)
    );

    //=====================================================================
    // MINIMAL TX STUB - idle beats only, but protocol-correct
    //
    // UG sec 7.4:
    //   "The i_tx_mac_valid signal asserts only when the o_tx_mac_ready
    //    signal is asserted, even though there is no packet to send."
    //   "...can be spaced by a fixed latency between 1 to 7 clock cycles."
    //   "When i_tx_mac_valid deasserts, [all TX signals] must be paused for
    //    as many cycles as o_tx_mac_ready is deasserted."
    //
    // So valid tracks READY delayed, never data availability. An idle beat
    // is inframe=0 with valid HIGH - it is NOT valid=0.
    //
    // This is the smallest legal TX driver, and it exercises the pause
    // protocol independently of any datapath.
    //=====================================================================
    reg [7:0] rdy_pipe;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) rdy_pipe <= 8'd0;
        else        rdy_pipe <= {rdy_pipe[6:0], tx_ready};
    end

    // Tap one stage early: the outputs below are REGISTERED, adding a cycle.
    // This makes tx_valid land exactly RDY_LAT cycles after tx_ready.
    wire tx_en = (RDY_LAT >= 2) ? rdy_pipe[RDY_LAT-2] : tx_ready;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tx_data <= 0; tx_valid <= 1'b0; tx_inframe <= 0;
            tx_eop_empty <= 0; tx_error <= 0; tx_skip_crc <= 0;
        end else begin
            tx_valid <= tx_en;
            if (tx_en) begin
                tx_data      <= 0;
                tx_inframe   <= 0;    // idle beat, valid stays HIGH
                tx_eop_empty <= 0;
                tx_error     <= 0;
                tx_skip_crc  <= 0;
            end
            // tx_en low -> everything holds. Bus frozen.
        end
    end

    //=====================================================================
    // RX INTERFACE DECODER - the point of this testbench
    //=====================================================================
    integer beats;             // beats with rx_valid asserted
    integer data_beats;        // beats with inframe != 0
    integer idle_beats;        // valid but inframe == 0
    integer seg_in_frame;      // segments carrying frame data
    integer eop_nonzero;       // segments with eop_empty != 0 (LOWER BOUND)
    integer fcs_errs;
    integer e_malformed, e_size, e_paylen;

    integer i;
    always @(posedge clk) begin
        if (!rst_n) begin
            beats = 0; data_beats = 0; idle_beats = 0;
            seg_in_frame = 0; eop_nonzero = 0; fcs_errs = 0;
            e_malformed = 0; e_size = 0; e_paylen = 0;
        end else if (rx_valid) begin
            beats = beats + 1;
            if (|rx_inframe) data_beats = data_beats + 1;
            else             idle_beats = idle_beats + 1;

            for (i = 0; i < NUM_SEG; i = i + 1) begin
                if (rx_inframe[i]) begin
                    seg_in_frame = seg_in_frame + 1;

                    // eop_empty != 0 is NOT a reliable EOP marker: a frame
                    // ending exactly on a segment boundary has eop_empty==0.
                    // A 64-byte frame is exactly 8 segments, so ALL its EOPs
                    // are invisible here. Lower bound only. The unambiguous
                    // EOP encoding under tight packing is the open question.
                    if (rx_eop_empty[i*EMPTY_W +: EMPTY_W] != 0)
                        eop_nonzero = eop_nonzero + 1;

                    if (rx_fcs_error[i]) fcs_errs = fcs_errs + 1;

                    // UG Table 46 rx_error codes, 2 bits per segment
                    case (rx_error[i*2 +: 2])
                        2'd1: e_malformed = e_malformed + 1;  // malformed
                        2'd2: e_size      = e_size + 1;       // under/oversize
                        2'd3: e_paylen    = e_paylen + 1;     // payload length
                    endcase
                end
            end
        end
    end

    //=====================================================================
    // TEST DRIVER
    //=====================================================================
    integer errors;
    integer frames_this_phase;
    real    frames_per_beat, seg_util;

    task reset_and_run;
        input integer cycles;
        input [15:0]  len;
        input [7:0]   stall;
        input         err;
        begin
            gen_enable = 1'b0;
            rst_n      = 1'b0;
            repeat (20) @(posedge clk);
            rst_n      = 1'b1;
            repeat (10) @(posedge clk);
            force_len    = len;
            stall_rate   = stall;
            inject_error = err;
            gen_enable   = 1'b1;
            repeat (cycles) @(posedge clk);
            gen_enable   = 1'b0;
            repeat (200) @(posedge clk);
            frames_this_phase = m_rx_frames;
        end
    endtask

    task report;
        input [400*8-1:0] label;
        begin
            frames_per_beat = (beats == 0) ? 0.0 :
                              (frames_this_phase * 1.0) / beats;
            seg_util        = (beats == 0) ? 0.0 :
                              (seg_in_frame * 100.0) / (beats * NUM_SEG);

            $display("--------------------------------------------------------");
            $display("%0s", label);
            $display("  rx_valid beats          : %0d", beats);
            $display("    with frame data       : %0d", data_beats);
            $display("    idle (inframe==0)     : %0d", idle_beats);
            $display("  frames received         : %0d", frames_this_phase);
            $display("  FRAMES PER BEAT         : %0.3f", frames_per_beat);
            $display("  segment utilisation     : %0.1f %%  (%0d of %0d)",
                     seg_util, seg_in_frame, beats*NUM_SEG);
            $display("  eop_empty != 0 segments : %0d  (lower bound only)",
                     eop_nonzero);
            $display("  fcs errors              : %0d", fcs_errs);
            $display("  rx_error malformed      : %0d", e_malformed);
            $display("  rx_error under/oversize : %0d", e_size);
            $display("  rx_error payload length : %0d", e_paylen);
            $display("  TX protocol violations  : %0d", m_viol);
        end
    endtask

    task check;
        input [200*8-1:0] what;
        input             cond;
        begin
            if (!cond) begin
                $display("  ** FAIL : %0s", what);
                errors = errors + 1;
            end else begin
                $display("  ok      : %0s", what);
            end
        end
    endtask

    //=====================================================================
    initial begin
        errors = 0;

        $display("========================================================");
        $display(" STAGE 1 - 400GE MAC SEGMENTED INTERFACE SMOKE TEST");
        $display(" Model only. No loopback, no FIFO, no pipeline.");
        $display(" 1024-bit, 16 x 8-byte segments, 415.0390625 MHz");
        $display("========================================================");

        //---------------- A: mixed-length frames ----------------
        reset_and_run(2000, 16'd0, 8'd0, 1'b0);
        report("A: mixed-length frames (64..1518B), TX always ready");
        check("traffic was generated",        beats > 1000);
        check("no idle beats during traffic", idle_beats == 0);
        check("segment utilisation > 99%",    seg_util > 99.0);
        check("no spurious errors",           (fcs_errs==0) && (e_malformed==0));
        check("no TX protocol violations",    m_viol == 0);

        //---------------- B: 64-byte frames ----------------
        // 64 bytes = 8 segments. A beat is 16 segments.
        // So EXACTLY TWO frames per beat. This is the condition UG sec 7.5
        // describes - "a new packet may start and the previous packet end
        // are within the same cycle" - and it is what breaks naive
        // datapaths that keep one packet state per beat.
        reset_and_run(2000, 16'd64, 8'd0, 1'b0);
        report("B: 64-byte frames - MULTI-FRAME-PER-BEAT PROOF");
        check("exactly 2.0 frames per beat",
              (frames_per_beat > 1.99) && (frames_per_beat < 2.01));
        check("100% segment utilisation",     seg_util > 99.9);
        check("no TX protocol violations",    m_viol == 0);
        $display("  NOTE: eop_empty!=0 count is 0 here and that is CORRECT.");
        $display("        A 64B frame ends exactly on a segment boundary, so");
        $display("        eop_empty is legitimately 0 at every EOP.");

        //---------------- C: maximum-length frames ----------------
        // 1518 bytes / 128 bytes per beat = 11.86 -> ~12 beats per frame
        reset_and_run(2000, 16'd1518, 8'd0, 1'b0);
        report("C: 1518-byte frames - approx 12 beats per frame");
        check("frames per beat approx 1/12",
              (frames_per_beat > 0.07) && (frames_per_beat < 0.09));
        check("segment utilisation > 99%",    seg_util > 99.0);
        check("every frame shows an EOP",     eop_nonzero == frames_this_phase);
        check("no TX protocol violations",    m_viol == 0);

        //---------------- D: TX pause protocol ----------------
        reset_and_run(2000, 16'd0, 8'd60, 1'b0);
        report("D: TX stalls - pause protocol, idle-only driver");
        check("no violations under stall",    m_viol == 0);
        check("RX unaffected (no backpressure)", beats > 1000);

        //---------------- E: RX error injection ----------------
        reset_and_run(2000, 16'd0, 8'd0, 1'b1);
        report("E: RX error injection - every frame flagged bad");
        check("fcs errors reported",          fcs_errs > 0);
        check("rx_error code 1 (malformed)",  e_malformed > 0);
        check("fcs and error counts agree",   fcs_errs == e_malformed);
        check("no TX protocol violations",    m_viol == 0);

        //---------------- summary ----------------
        $display("========================================================");
        if (errors == 0) $display(" STAGE 1 RESULT: ALL CHECKS PASSED");
        else             $display(" STAGE 1 RESULT: %0d CHECK(S) FAILED", errors);
        $display("========================================================");
        $display(" Verified here: signal widths, tight packing, multi-frame-");
        $display(" per-beat, RX error codes, TX pause protocol.");
        $display("");
        $display(" NOT verified at this stage: reset/status sequencing, any");
        $display(" datapath, data integrity, PMA/PCS/FEC/link training.");
        $display("");
        $display(" Next: STAGE 2 - connect eth400g_loopback and run");
        $display("       tb_eth400g_top for beat-exact data integrity.");
        $display("========================================================");
        $finish;
    end

    //=====================================================================
    initial begin
        $dumpfile("tb_iface_only.vcd");
        $dumpvars(0, tb_iface_only);
    end

    initial begin
        #200000000;
        $display("** TIMEOUT");
        $finish;
    end

endmodule

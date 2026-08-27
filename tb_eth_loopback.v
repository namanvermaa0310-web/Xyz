//=============================================================================
// tb_eth_loopback.v   -- Verilog-2001
//
// Self-checking testbench for eth_loopback.v
//
// Generator and checker are inline here (not separate modules) to keep this
// to two files total. They are TEST code, not part of your design.
//
// WHAT THIS PROVES:   your fabric logic - framing, empty-field handling,
//                     FIFO behaviour, backpressure compliance.
// WHAT IT DOES NOT:   PMA, PCS, RS-FEC, alignment marker lock, link training.
//                     Those need the real IP simulated, then hardware.
//=============================================================================
`timescale 1ps/1ps

module tb_eth_loopback;

    localparam DATA_W  = 1024;
    localparam NUM_SEG = 16;
    localparam EMPTY_W = 3;
    localparam SEG_B   = 8;               // bytes per segment
    localparam BEAT_B  = NUM_SEG*SEG_B;   // 128 bytes per beat

    reg clk = 1'b0;
    reg rst_n = 1'b0;
    always #1205 clk = ~clk;              // ~415 MHz

    // ---- RX stimulus into DUT ----
    reg  [DATA_W-1:0]          rx_data;
    reg  [NUM_SEG-1:0]         rx_valid, rx_sop, rx_eop, rx_error;
    reg  [NUM_SEG*EMPTY_W-1:0] rx_empty;

    // ---- TX output from DUT ----
    wire [DATA_W-1:0]          tx_data;
    wire [NUM_SEG-1:0]         tx_valid, tx_sop, tx_eop, tx_error;
    wire [NUM_SEG*EMPTY_W-1:0] tx_empty;
    reg                        tx_ready;

    wire [31:0] rx_pkt_cnt, tx_pkt_cnt, drop_cnt;

    eth_loopback #(
        .DATA_W(DATA_W), .NUM_SEG(NUM_SEG), .EMPTY_W(EMPTY_W),
        .FIFO_DEPTH(512), .ADDR_W(9)
    ) dut (
        .clk(clk), .rst_n(rst_n),
        .rx_data(rx_data), .rx_valid(rx_valid), .rx_sop(rx_sop),
        .rx_eop(rx_eop), .rx_empty(rx_empty), .rx_error(rx_error),
        .tx_data(tx_data), .tx_valid(tx_valid), .tx_sop(tx_sop),
        .tx_eop(tx_eop), .tx_empty(tx_empty), .tx_error(tx_error),
        .tx_ready(tx_ready),
        .rx_pkt_cnt(rx_pkt_cnt), .tx_pkt_cnt(tx_pkt_cnt), .drop_cnt(drop_cnt)
    );

    //-----------------------------------------------------------------------
    // Expected byte value at offset `idx` of frame with sequence number `sq`
    //-----------------------------------------------------------------------
    function [7:0] frame_byte;
        input integer idx;
        input [31:0]  sq;
        begin
            if (idx < 6)       frame_byte = 8'hFF;              // DA broadcast
            else if (idx == 6) frame_byte = 8'h00;              // SA
            else if (idx == 7) frame_byte = 8'h1A;
            else if (idx == 8) frame_byte = 8'h2B;
            else if (idx == 9) frame_byte = 8'h3C;
            else if (idx ==10) frame_byte = 8'h4D;
            else if (idx ==11) frame_byte = 8'h5E;
            else if (idx ==12) frame_byte = 8'h08;              // type hi
            else if (idx ==13) frame_byte = 8'h00;              // type lo
            else if (idx ==14) frame_byte = sq[31:24];          // seq
            else if (idx ==15) frame_byte = sq[23:16];
            else if (idx ==16) frame_byte = sq[15:8];
            else if (idx ==17) frame_byte = sq[7:0];
            else               frame_byte = (idx & 8'hFF) ^ sq[7:0];
        end
    endfunction

    //-----------------------------------------------------------------------
    // Driver: push one frame of `len` bytes into RX
    //-----------------------------------------------------------------------
    integer sent_cnt;
    reg [31:0] tx_seq;

    task send_frame;
        input integer len;
        input         mark_error;
        integer off, s, b, base, nbytes;
        begin
            off = 0;
            while (off < len) begin
                @(negedge clk);
                rx_data  = {DATA_W{1'b0}};
                rx_valid = {NUM_SEG{1'b0}};
                rx_sop   = {NUM_SEG{1'b0}};
                rx_eop   = {NUM_SEG{1'b0}};
                rx_empty = {(NUM_SEG*EMPTY_W){1'b0}};
                rx_error = {NUM_SEG{1'b0}};

                for (s = 0; s < NUM_SEG; s = s + 1) begin
                    base = off + s*SEG_B;
                    if (base < len) begin
                        rx_valid[s] = 1'b1;
                        for (b = 0; b < SEG_B; b = b + 1) begin
                            if ((base + b) < len)
                                rx_data[(s*SEG_B + b)*8 +: 8] =
                                    frame_byte(base + b, tx_seq);
                        end
                        if (base == 0) rx_sop[s] = 1'b1;
                        if ((base + SEG_B) >= len) begin
                            rx_eop[s] = 1'b1;
                            nbytes = len - base;
                            rx_empty[s*EMPTY_W +: EMPTY_W] = SEG_B - nbytes;
                            if (mark_error) rx_error[s] = 1'b1;
                        end
                    end
                end
                off = off + BEAT_B;
            end

            @(negedge clk);
            rx_valid = {NUM_SEG{1'b0}};
            rx_sop   = {NUM_SEG{1'b0}};
            rx_eop   = {NUM_SEG{1'b0}};
            rx_error = {NUM_SEG{1'b0}};

            tx_seq   = tx_seq + 1;
            sent_cnt = sent_cnt + 1;
        end
    endtask

    //-----------------------------------------------------------------------
    // Checker: watch the TX side and validate every frame
    //-----------------------------------------------------------------------
    integer rcvd_cnt, data_err, len_err, seq_err;
    reg [31:0] exp_seq, cap_seq;
    reg [15:0] cur_bytes;
    reg        first_frame, frame_bad;

    wire t_any_valid = |tx_valid;
    wire t_any_sop   = |(tx_sop & tx_valid);
    wire t_any_eop   = |(tx_eop & tx_valid);

    wire [31:0] t_seq_field = { tx_data[14*8 +: 8], tx_data[15*8 +: 8],
                                tx_data[16*8 +: 8], tx_data[17*8 +: 8] };

    integer cs, cb, coff, cn;
    reg [15:0] base_off, final_len, beat_bytes;
    reg [31:0] use_seq;
    reg        beat_bad;      // computed with blocking assigns this cycle
    reg        acc_bad;       // running per-frame flag

    always @(posedge clk) begin
        if (!rst_n) begin
            rcvd_cnt <= 0; data_err <= 0; len_err <= 0; seq_err <= 0;
            exp_seq  <= 0; cap_seq  <= 0; cur_bytes <= 0;
            first_frame <= 1'b1; frame_bad <= 1'b0;
        end else if (t_any_valid && tx_ready) begin

            base_off = t_any_sop ? 16'd0 : cur_bytes;
            use_seq  = t_any_sop ? t_seq_field : cap_seq;

            if (t_any_sop) begin
                cap_seq <= t_seq_field;
                if (!first_frame && (t_seq_field !== exp_seq))
                    seq_err <= seq_err + 1;
                first_frame <= 1'b0;
            end

            // Byte-by-byte compare + count bytes in this beat.
            // Use BLOCKING assignments so the result is valid within this
            // same cycle - a non-blocking flag would race with the EOP check.
            beat_bytes = 0;
            beat_bad   = 1'b0;
            for (cs = 0; cs < NUM_SEG; cs = cs + 1) begin
                if (tx_valid[cs]) begin
                    cn = tx_eop[cs] ? (SEG_B - tx_empty[cs*EMPTY_W +: EMPTY_W])
                                    : SEG_B;
                    beat_bytes = beat_bytes + cn;
                    for (cb = 0; cb < SEG_B; cb = cb + 1) begin
                        if (cb < cn) begin
                            coff = base_off + cs*SEG_B + cb;
                            if (tx_data[(cs*SEG_B + cb)*8 +: 8] !==
                                frame_byte(coff, use_seq))
                                beat_bad = 1'b1;
                        end
                    end
                end
            end

            // Accumulate the per-frame bad flag (cleared at SOP)
            acc_bad = t_any_sop ? beat_bad : (frame_bad | beat_bad);
            frame_bad <= acc_bad;

            cur_bytes <= base_off + beat_bytes;

            if (t_any_eop) begin
                final_len = base_off + beat_bytes;
                rcvd_cnt <= rcvd_cnt + 1;
                exp_seq  <= use_seq + 1;
                if (acc_bad)                            data_err <= data_err + 1;
                if (final_len < 64 || final_len > 1518) len_err  <= len_err + 1;
            end
        end
    end

    //-----------------------------------------------------------------------
    // Test sequence
    //-----------------------------------------------------------------------
    integer i, errors;
    reg [7:0] bp_lfsr;

    // Random backpressure generator
    always @(posedge clk) begin
        if (!rst_n) bp_lfsr <= 8'hA5;
        else        bp_lfsr <= {bp_lfsr[6:0], bp_lfsr[7]^bp_lfsr[5]};
    end

    task do_reset;
        begin
            rst_n = 1'b0;
            rx_valid = 0; rx_sop = 0; rx_eop = 0; rx_error = 0;
            rx_data = 0; rx_empty = 0;
            tx_seq = 0; sent_cnt = 0;
            repeat (20) @(posedge clk);
            rst_n = 1'b1;
            repeat (10) @(posedge clk);
        end
    endtask

    initial begin
        errors = 0;
        tx_ready = 1'b1;

        //---------------- PHASE 1: clean loopback ----------------
        do_reset;
        for (i = 0; i < 50; i = i + 1)
            send_frame(64 + (i*37) % 1400, 1'b0);
        repeat (3000) @(posedge clk);

        $display("--------------------------------------------------");
        $display("PHASE 1: clean loopback (no backpressure)");
        $display("  frames sent      : %0d", sent_cnt);
        $display("  dut rx / tx      : %0d / %0d", rx_pkt_cnt, tx_pkt_cnt);
        $display("  dut drops        : %0d", drop_cnt);
        $display("  checker received : %0d", rcvd_cnt);
        $display("  data/len/seq err : %0d / %0d / %0d",
                 data_err, len_err, seq_err);
        if (rcvd_cnt != sent_cnt) begin
            $display("  ** FAIL: frame count mismatch"); errors = errors + 1;
        end
        if (data_err != 0) begin
            $display("  ** FAIL: data corruption"); errors = errors + 1;
        end
        if (len_err != 0) begin
            $display("  ** FAIL: length errors (check empty handling)");
            errors = errors + 1;
        end
        if (seq_err != 0) begin
            $display("  ** FAIL: sequence errors"); errors = errors + 1;
        end

        //---------------- PHASE 2: TX backpressure ----------------
        do_reset;
        fork
            begin : bp_drive
                integer k;
                for (k = 0; k < 100000; k = k + 1) begin
                    @(negedge clk);              // stable before posedge sample
                    tx_ready = bp_lfsr[0];
                end
            end
            begin : traffic
                for (i = 0; i < 30; i = i + 1)
                    send_frame(64 + (i*53) % 1400, 1'b0);
                repeat (5000) @(posedge clk);
                disable bp_drive;
            end
        join
        tx_ready = 1'b1;
        repeat (2000) @(posedge clk);

        $display("--------------------------------------------------");
        $display("PHASE 2: random TX backpressure");
        $display("  frames sent      : %0d", sent_cnt);
        $display("  dut rx / tx      : %0d / %0d", rx_pkt_cnt, tx_pkt_cnt);
        $display("  dut drops        : %0d", drop_cnt);
        $display("  checker received : %0d", rcvd_cnt);
        $display("  data/len err     : %0d / %0d", data_err, len_err);
        // Drops under backpressure are legitimate. Corruption is not.
        if (data_err != 0) begin
            $display("  ** FAIL: data corruption under backpressure");
            errors = errors + 1;
        end
        if (len_err != 0) begin
            $display("  ** FAIL: truncated frames under backpressure");
            errors = errors + 1;
        end

        //---------------- PHASE 3: RX error injection ----------------
        do_reset;
        for (i = 0; i < 20; i = i + 1)
            send_frame(64 + (i*61) % 1400, 1'b1);   // every frame flagged bad
        repeat (3000) @(posedge clk);

        $display("--------------------------------------------------");
        $display("PHASE 3: RX error injection (all frames bad)");
        $display("  dut drops        : %0d", drop_cnt);
        $display("  checker received : %0d  (expect 0)", rcvd_cnt);
        if (drop_cnt == 0) begin
            $display("  ** FAIL: errored frames not dropped");
            errors = errors + 1;
        end
        if (rcvd_cnt != 0) begin
            $display("  ** FAIL: errored frames leaked through");
            errors = errors + 1;
        end

        $display("==================================================");
        if (errors == 0) $display("RESULT: ALL PHASES PASSED");
        else             $display("RESULT: %0d FAILURE(S)", errors);
        $display("==================================================");
        $display("NOTE: fabric logic only. PMA/PCS/FEC/link training are");
        $display("NOT covered - simulate the real IP, then test hardware.");
        $finish;
    end

    initial begin
        #2000000000;
        $display("** TIMEOUT");
        $finish;
    end

endmodule

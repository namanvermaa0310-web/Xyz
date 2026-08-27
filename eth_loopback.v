//=============================================================================
// eth_loopback.v      -- Verilog-2001
//
// Simple Ethernet RX -> TX loopback for the F-Tile Ethernet Hard IP
// segmented client interface (400GbE, 1024-bit datapath, 16 x 64-bit segments).
//
// WHY THIS IS NOT JUST A WIRE
// ---------------------------
//   RX has NO backpressure - data arrives whether you are ready or not.
//   TX HAS backpressure    - it can stall you with tx_ready.
//   The MAC also requires CONTIGUOUS transfer: tx_valid must stay asserted
//   from SOP through EOP with no gaps.
//
//   So a direct RX->TX wire loses data the moment TX stalls, and emits
//   corrupted frames if it stalls mid-packet.
//
//   This module buffers a COMPLETE packet, then transmits it. That is the
//   minimum needed for a loopback that actually works.
//
// ASSUMPTION: rx and tx client clocks are the SAME clock.
//   That is the normal case for a loopback (both come from the same IP).
//   If they are genuinely separate clocks you need gray-coded CDC pointers.
//=============================================================================
`timescale 1ps/1ps

module eth_loopback #(
    parameter DATA_W     = 1024,          // client datapath width
    parameter NUM_SEG    = 16,            // segments per beat
    parameter EMPTY_W    = 3,             // empty bits per segment
    parameter FIFO_DEPTH = 512,           // beats
    parameter ADDR_W     = 9              // must equal log2(FIFO_DEPTH)
)(
    input  wire                        clk,
    input  wire                        rst_n,

    // ---- RX client interface (from Ethernet IP) ----
    input  wire [DATA_W-1:0]           rx_data,
    input  wire [NUM_SEG-1:0]          rx_valid,
    input  wire [NUM_SEG-1:0]          rx_sop,
    input  wire [NUM_SEG-1:0]          rx_eop,
    input  wire [NUM_SEG*EMPTY_W-1:0]  rx_empty,
    input  wire [NUM_SEG-1:0]          rx_error,

    // ---- TX client interface (to Ethernet IP) ----
    output reg  [DATA_W-1:0]           tx_data,
    output reg  [NUM_SEG-1:0]          tx_valid,
    output reg  [NUM_SEG-1:0]          tx_sop,
    output reg  [NUM_SEG-1:0]          tx_eop,
    output reg  [NUM_SEG*EMPTY_W-1:0]  tx_empty,
    output reg  [NUM_SEG-1:0]          tx_error,
    input  wire                        tx_ready,

    // ---- Status counters ----
    output reg  [31:0]                 rx_pkt_cnt,
    output reg  [31:0]                 tx_pkt_cnt,
    output reg  [31:0]                 drop_cnt
);

    // One FIFO entry packs the whole beat
    localparam ENTRY_W = DATA_W + NUM_SEG*4 + NUM_SEG*EMPTY_W;

    reg [ENTRY_W-1:0] mem [0:FIFO_DEPTH-1];

    reg [ADDR_W:0] wr_ptr;
    reg [ADDR_W:0] rd_ptr;
    reg [ADDR_W:0] sop_ptr;      // snapshot for rewind on aborted frame

    // NOTE: a single pkt_count driven from both the RX and TX always blocks
    // would be a multiple-driver error. Use two counters and compare.
    reg [15:0] pkt_wr;           // complete packets committed by RX
    reg [15:0] pkt_rd;           // complete packets consumed by TX
    wire       pkt_avail = (pkt_wr != pkt_rd);
    reg        in_pkt;
    reg        drop_pkt;

    wire full  = (wr_ptr[ADDR_W] != rd_ptr[ADDR_W]) &&
                 (wr_ptr[ADDR_W-1:0] == rd_ptr[ADDR_W-1:0]);
    wire empty = (wr_ptr == rd_ptr);

    wire any_valid = |rx_valid;
    wire any_sop   = |(rx_sop   & rx_valid);
    wire any_eop   = |(rx_eop   & rx_valid);
    wire any_err   = |(rx_error & rx_valid);

    wire [ENTRY_W-1:0] wr_entry = {rx_data, rx_valid, rx_sop, rx_eop,
                                   rx_error, rx_empty};

    //-------------------------------------------------------------------
    // RX side: write complete packets into the FIFO
    //-------------------------------------------------------------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wr_ptr     <= 0;
            sop_ptr    <= 0;
            pkt_wr     <= 0;
            in_pkt     <= 1'b0;
            drop_pkt   <= 1'b0;
            rx_pkt_cnt <= 0;
            drop_cnt   <= 0;
        end else begin

            // Start of frame: remember where it began so we can rewind
            if (any_valid && any_sop) begin
                in_pkt   <= 1'b1;
                sop_ptr  <= wr_ptr;
                drop_pkt <= full;
            end

            // Ran out of room part way through -> abandon this frame.
            // Without this you write a TRUNCATED packet and still commit it,
            // which is silent corruption that only shows up under load.
            if (any_valid && !any_sop && !drop_pkt && full)
                drop_pkt <= 1'b1;

            // Write beats while the frame is still viable
            if (any_valid && !drop_pkt && !full) begin
                mem[wr_ptr[ADDR_W-1:0]] <= wr_entry;
                wr_ptr <= wr_ptr + 1'b1;
            end

            // End of frame
            if (any_valid && any_eop) begin
                in_pkt     <= 1'b0;
                drop_pkt   <= 1'b0;
                rx_pkt_cnt <= rx_pkt_cnt + 1'b1;

                if (drop_pkt || any_err) begin
                    wr_ptr   <= sop_ptr;              // rewind - discard frame
                    drop_cnt <= drop_cnt + 1'b1;
                end else begin
                    pkt_wr <= pkt_wr + 1'b1;          // commit complete frame
                end
            end
        end
    end

    //-------------------------------------------------------------------
    // TX side: send a packet only once it is fully buffered
    //-------------------------------------------------------------------
    // Single registered output stage with a proper ready/valid handshake.
    //
    // The output register is free to accept a new beat when it is either empty
    // or its current beat has just been taken (tx_ready high). A beat that is
    // NOT taken is held unchanged - tx_valid stays asserted, which is the
    // contiguous-transfer behaviour the MAC requires.
    reg tx_vld_r;      // a beat is currently presented
    reg sending;       // mid-packet

    wire out_free = ~tx_vld_r | tx_ready;

    // Asynchronous FIFO read (distributed-RAM style; Quartus infers MLAB/M20K)
    wire [ENTRY_W-1:0] dout     = mem[rd_ptr[ADDR_W-1:0]];
    wire [NUM_SEG-1:0] dout_eop = dout[NUM_SEG*EMPTY_W + NUM_SEG   +: NUM_SEG];
    wire [NUM_SEG-1:0] dout_val = dout[NUM_SEG*EMPTY_W + NUM_SEG*3 +: NUM_SEG];
    wire               dout_last = |(dout_eop & dout_val);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rd_ptr     <= 0;
            pkt_rd     <= 0;
            tx_pkt_cnt <= 0;
            tx_vld_r   <= 1'b0;
            sending    <= 1'b0;
            tx_data    <= 0;
            tx_valid   <= 0;
            tx_sop     <= 0;
            tx_eop     <= 0;
            tx_empty   <= 0;
            tx_error   <= 0;
        end else if (out_free) begin

            if (sending && !empty) begin
                // Continue the packet already in flight
                {tx_data, tx_valid, tx_sop, tx_eop, tx_error, tx_empty} <= dout;
                tx_vld_r <= 1'b1;
                rd_ptr   <= rd_ptr + 1'b1;
                if (dout_last) begin
                    sending    <= 1'b0;
                    tx_pkt_cnt <= tx_pkt_cnt + 1'b1;
                    pkt_rd     <= pkt_rd + 1'b1;
                end

            end else if (!sending && pkt_avail && !empty) begin
                // Start a new packet - only ever when a COMPLETE one is buffered
                {tx_data, tx_valid, tx_sop, tx_eop, tx_error, tx_empty} <= dout;
                tx_vld_r <= 1'b1;
                rd_ptr   <= rd_ptr + 1'b1;
                if (dout_last) begin
                    tx_pkt_cnt <= tx_pkt_cnt + 1'b1;
                    pkt_rd     <= pkt_rd + 1'b1;
                end else begin
                    sending <= 1'b1;
                end

            end else begin
                // Nothing to send - deassert
                tx_vld_r <= 1'b0;
                tx_valid <= 0;
                tx_sop   <= 0;
                tx_eop   <= 0;
            end
        end
        // If !out_free everything holds - beat stays presented until taken.
    end

endmodule

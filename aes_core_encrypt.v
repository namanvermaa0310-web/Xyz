// =============================================================
// Module : aes_core_encrypt
// Purpose: Full AES-128 encryption
// Fixed  : ke_done stale-high bug — added ke_busy flag
//          ke_done only checked AFTER ke_start was pulsed
// Target : Cyclone 10 LP / ModelSim-Altera 10.5b (Verilog-2001)
// =============================================================

module aes_core_encrypt (
    input  wire          clk,
    input  wire          rst_n,
    input  wire          start,
    input  wire [127:0]  key_in,
    input  wire [127:0]  plaintext_in,
    output reg  [127:0]  ciphertext_out,
    output reg           done
);

    // Latched inputs
    reg [127:0] saved_key;
    reg [127:0] saved_pt;

    // Key expansion
    reg          ke_start;
    reg          ke_busy;   // set when we fire ke_start, clear when ke_done seen
    wire [1407:0] round_keys_flat;
    wire          ke_done;

    aes_key_expand u_key_expand (
        .clk             (clk),
        .rst_n           (rst_n),
        .start           (ke_start),
        .key_in          (saved_key),
        .round_keys_flat (round_keys_flat),
        .done            (ke_done)
    );

    // Round key extraction
    wire [127:0] rk [0:10];
    assign rk[0]  = round_keys_flat[1407:1280];
    assign rk[1]  = round_keys_flat[1279:1152];
    assign rk[2]  = round_keys_flat[1151:1024];
    assign rk[3]  = round_keys_flat[1023:896];
    assign rk[4]  = round_keys_flat[895:768];
    assign rk[5]  = round_keys_flat[767:640];
    assign rk[6]  = round_keys_flat[639:512];
    assign rk[7]  = round_keys_flat[511:384];
    assign rk[8]  = round_keys_flat[383:256];
    assign rk[9]  = round_keys_flat[255:128];
    assign rk[10] = round_keys_flat[127:0];

    // Round module (combinational)
    reg  [127:0] round_state_in;
    reg  [127:0] round_key_sel;
    reg          round_last;
    wire [127:0] round_state_out;

    aes_round u_round (
        .state_in  (round_state_in),
        .round_key (round_key_sel),
        .last_round(round_last),
        .state_out (round_state_out)
    );

    // =========================================================
    // State machine
    // =========================================================
    localparam ST_IDLE    = 3'd0;
    localparam ST_LATCH   = 3'd1;
    localparam ST_KEYGEN  = 3'd2;
    localparam ST_ARK     = 3'd3;
    localparam ST_SETUP   = 3'd4;
    localparam ST_CAPTURE = 3'd5;
    localparam ST_DONE    = 3'd6;

    reg [2:0]   state;
    reg [3:0]   round_num;
    reg [127:0] cur_state;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state          <= ST_IDLE;
            done           <= 0;
            ke_start       <= 0;
            ke_busy        <= 0;
            round_num      <= 0;
            cur_state      <= 0;
            saved_key      <= 0;
            saved_pt       <= 0;
            ciphertext_out <= 0;
            round_state_in <= 0;
            round_key_sel  <= 0;
            round_last     <= 0;
        end
        else begin
            ke_start <= 0; // default deasserted

            case (state)

                ST_IDLE: begin
                    done     <= 0;
                    ke_busy  <= 0;
                    if (start) begin
                        saved_key <= key_in;
                        saved_pt  <= plaintext_in;
                        state     <= ST_LATCH;
                    end
                end

                // One cycle for saved_key to settle
                ST_LATCH: begin
                    ke_start <= 1;    // pulse ke_start exactly once
                    ke_busy  <= 1;    // mark expansion in progress
                    state    <= ST_KEYGEN;
                end

                // Wait for fresh ke_done — only valid when ke_busy=1
                ST_KEYGEN: begin
                    if (ke_busy && ke_done) begin
                        ke_busy   <= 0;
                        cur_state <= saved_pt ^ rk[0];
                        state     <= ST_ARK;
                    end
                end

                // Drive round 1 inputs
                ST_ARK: begin
                    round_state_in <= cur_state;
                    round_key_sel  <= rk[1];
                    round_last     <= 0;
                    round_num      <= 1;
                    state          <= ST_CAPTURE;
                end

                ST_SETUP: begin
                    round_state_in <= cur_state;
                    round_key_sel  <= rk[round_num];
                    round_last     <= (round_num == 10) ? 1'b1 : 1'b0;
                    state          <= ST_CAPTURE;
                end

                ST_CAPTURE: begin
                    cur_state <= round_state_out;
                    if (round_num == 10) begin
                        state <= ST_DONE;
                    end else begin
                        round_num <= round_num + 1;
                        state     <= ST_SETUP;
                    end
                end

                ST_DONE: begin
                    ciphertext_out <= cur_state;
                    done           <= 1;
                    state          <= ST_IDLE;
                end

                default: state <= ST_IDLE;
            endcase
        end
    end

endmodule

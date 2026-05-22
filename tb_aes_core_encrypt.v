// =============================================================
// Testbench : tb_aes_core_encrypt
// Fixed     : Clean synchronous handshaking between tests
//             key/plaintext stable BEFORE start pulse
//             wait for done to go LOW before next test
// Tool      : ModelSim-Altera 10.5b (Verilog-2001)
// =============================================================

`timescale 1ns/1ps

module tb_aes_core_encrypt;

    reg          clk;
    reg          rst_n;
    reg          start;
    reg  [127:0] key_in;
    reg  [127:0] plaintext_in;
    wire [127:0] ciphertext_out;
    wire         done;

    aes_core_encrypt dut (
        .clk           (clk),
        .rst_n         (rst_n),
        .start         (start),
        .key_in        (key_in),
        .plaintext_in  (plaintext_in),
        .ciphertext_out(ciphertext_out),
        .done          (done)
    );

    // 10ns clock
    initial clk = 0;
    always #5 clk = ~clk;

    integer pass_count;
    integer fail_count;

    task check;
        input [127:0] got;
        input [127:0] exp;
        begin
            if (got === exp) begin
                $display("PASS: %h", got);
                pass_count = pass_count + 1;
            end else begin
                $display("FAIL:");
                $display("  got     : %h", got);
                $display("  expected: %h", exp);
                fail_count = fail_count + 1;
            end
        end
    endtask

    // Task: run one encryption cleanly
    // key and plaintext must be set BEFORE calling this
    task run_encrypt;
        begin
            // Make sure start is low and done is low first
            start = 0;

            // Wait for a clean clock edge
            @(posedge clk); #2;

            // Pulse start for exactly one cycle
            start = 1;
            @(posedge clk); #2;
            start = 0;

            // Wait for done to go high
            @(posedge done);
            // Give one more cycle for output to settle
            @(posedge clk); #2;

            // Wait for done to go low before next test
            @(negedge done);
            @(posedge clk); #2;
        end
    endtask

    initial begin
        // Initialise
        rst_n        = 0;
        start        = 0;
        pass_count   = 0;
        fail_count   = 0;
        key_in       = 128'h0;
        plaintext_in = 128'h0;

        // Reset for 3 cycles
        repeat(3) @(posedge clk);
        #2; rst_n = 1;
        repeat(2) @(posedge clk); #2;

        $display("========================================");
        $display("  AES-128 Core Encrypt Test");
        $display("  Reference: FIPS-197 / NIST AESAVS");
        $display("========================================");

        // -------------------------------------------------
        // TEST 1: FIPS-197
        // Key      : 2b7e151628aed2a6abf7158809cf4f3c
        // Plaintext: 3243f6a8885a308d313198a2e0370734
        // Expected : 3925841d02dc09fbdc118597196a0b32
        // -------------------------------------------------
        $display("--- Test 1: FIPS-197 vector ---");
        key_in       = 128'h2b7e151628aed2a6abf7158809cf4f3c;
        plaintext_in = 128'h3243f6a8885a308d313198a2e0370734;
        // Let inputs settle for 2 cycles before start
        repeat(2) @(posedge clk); #2;

        start = 1;
        @(posedge clk); #2;
        start = 0;

        wait(done == 1);
        @(posedge clk); #2;
        $display("  Key      : %h", key_in);
        $display("  Plaintext: %h", plaintext_in);
        $display("  Cipher   : %h", ciphertext_out);
        check(ciphertext_out, 128'h3925841d02dc09fbdc118597196a0b32);

        // Wait for done to go low (DUT back to IDLE)
        wait(done == 0);
        repeat(3) @(posedge clk); #2;

        // -------------------------------------------------
        // TEST 2: NIST zero key
        // Key      : 00000000000000000000000000000000
        // Plaintext: f34481ec3cc627bacd5dc3fb08f273e6
        // Expected : 0336763e966d92595a567cc9ce537f5e
        // -------------------------------------------------
        $display("--- Test 2: NIST zero key vector ---");
        key_in       = 128'h00000000000000000000000000000000;
        plaintext_in = 128'hf34481ec3cc627bacd5dc3fb08f273e6;
        // Let inputs settle for 2 cycles before start
        repeat(2) @(posedge clk); #2;

        start = 1;
        @(posedge clk); #2;
        start = 0;

        wait(done == 1);
        @(posedge clk); #2;
        $display("  Key      : %h", key_in);
        $display("  Plaintext: %h", plaintext_in);
        $display("  Cipher   : %h", ciphertext_out);
        check(ciphertext_out, 128'h0336763e966d92595a567cc9ce537f5e);

        // Wait for done to go low
        wait(done == 0);
        repeat(3) @(posedge clk); #2;

        // -------------------------------------------------
        // TEST 3: All zeros
        // Key      : 00000000000000000000000000000000
        // Plaintext: 00000000000000000000000000000000
        // Expected : 66e94bd4ef8a2c3b884cfa59ca342b2e
        // -------------------------------------------------
        $display("--- Test 3: All zeros ---");
        key_in       = 128'h00000000000000000000000000000000;
        plaintext_in = 128'h00000000000000000000000000000000;
        repeat(2) @(posedge clk); #2;

        start = 1;
        @(posedge clk); #2;
        start = 0;

        wait(done == 1);
        @(posedge clk); #2;
        $display("  Key      : %h", key_in);
        $display("  Plaintext: %h", plaintext_in);
        $display("  Cipher   : %h", ciphertext_out);
        check(ciphertext_out, 128'h66e94bd4ef8a2c3b884cfa59ca342b2e);

        wait(done == 0);
        repeat(2) @(posedge clk); #2;

        // -------------------------------------------------
        // Results
        // -------------------------------------------------
        $display("----------------------------------------");
        if (fail_count == 0)
            $display("  RESULT: ALL %0d TESTS PASSED", pass_count);
        else
            $display("  RESULT: %0d PASSED, %0d FAILED",
                      pass_count, fail_count);
        $display("========================================");
        $finish;
    end

    // Timeout watchdog — 100us
    initial begin
        #100000;
        $display("TIMEOUT - simulation hung");
        $finish;
    end

endmodule

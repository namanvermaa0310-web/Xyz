`timescale 1ns/1ps
// Minimal PicoRV32 SoC testbench:
//   - 64KB word RAM at 0x00000000 (loaded from firmware.hex)
//   - MMIO 0x10000000 : write a byte -> print as a character
//   - MMIO 0x10000004 : write       -> end simulation
module tb_picosoc;
    reg clk = 0, resetn = 0;
    always #5 clk = ~clk;          // 100 MHz

    wire        mem_valid, mem_instr;
    reg         mem_ready;
    wire [31:0] mem_addr, mem_wdata;
    wire [3:0]  mem_wstrb;
    reg  [31:0] mem_rdata;

    picorv32 #(
        .PROGADDR_RESET(32'h0000_0000),
        .STACKADDR     (32'h0001_0000)
    ) cpu (
        .clk(clk), .resetn(resetn),
        .mem_valid(mem_valid), .mem_instr(mem_instr), .mem_ready(mem_ready),
        .mem_addr(mem_addr), .mem_wdata(mem_wdata), .mem_wstrb(mem_wstrb),
        .mem_rdata(mem_rdata),
        // unused optional ports tied off
        .mem_la_read(), .mem_la_write(), .mem_la_addr(),
        .mem_la_wdata(), .mem_la_wstrb(),
        .pcpi_valid(), .pcpi_insn(), .pcpi_rs1(), .pcpi_rs2(),
        .pcpi_wr(1'b0), .pcpi_rd(32'b0), .pcpi_wait(1'b0), .pcpi_ready(1'b0),
        .irq(32'b0), .eoi(), .trace_valid(), .trace_data()
    );

    reg [31:0] ram [0:16383];      // 64 KB
    integer i;
    initial begin
        for (i=0;i<16384;i=i+1) ram[i]=32'h0;
        $readmemh("firmware.hex", ram);
    end

    wire [13:0] word_idx = mem_addr[15:2];

    always @(posedge clk) begin
        mem_ready <= 1'b0;
        if (mem_valid && !mem_ready) begin
            mem_ready <= 1'b1;
            if (mem_addr < 32'h0001_0000) begin            // RAM
                mem_rdata <= ram[word_idx];
                if (mem_wstrb[0]) ram[word_idx][7:0]   <= mem_wdata[7:0];
                if (mem_wstrb[1]) ram[word_idx][15:8]  <= mem_wdata[15:8];
                if (mem_wstrb[2]) ram[word_idx][23:16] <= mem_wdata[23:16];
                if (mem_wstrb[3]) ram[word_idx][31:24] <= mem_wdata[31:24];
            end
            else if (mem_addr == 32'h1000_0000) begin      // print port
                if (mem_wstrb) $write("%c", mem_wdata[7:0]);
            end
            else if (mem_addr == 32'h1000_0004) begin      // halt port
                $display("\n[sim] HALT - firmware finished.");
                $finish;
            end
            else mem_rdata <= 32'h0;
        end
    end

    initial begin
        repeat(4) @(posedge clk);
        resetn <= 1'b1;
        #100000 $display("\n[sim] TIMEOUT"); $finish;   // safety net
    end
endmodule

# USB Project — Line-by-Line Walkthrough
## Phase 5: Link Training in Verilog (`usb2_bridge_rsrw_verilog.v`)

> Two things are new here: (1) it's **Verilog**, not VHDL — so first a quick
> VHDL→Verilog translation table — and (2) it works at the **electrical link
> level** (wiggling wires to negotiate speed), *below* everything in Phases 1–4.

### Two honest flags before we start

1. **This is a vendor file.** Line 1: `// Copyright L&T Technology Services
   2016`. You did not write this module — it's third-party IP (LTTS). In an
   interview, say so plainly: "the link-training block is vendor IP; I
   integrated it and understand its interface, but didn't author it." Claiming
   authorship of a file with someone else's copyright header is the kind of
   thing that ends interviews.
2. **I previously called line 1643 a syntax error. I was wrong** — I re-checked
   against the actual file; it's a legal `if/else-if` chain, it compiles. Ignore
   that earlier claim.

## The one-line interview answer

> "This is the link-training block. Before any USB data can flow, the two ends
> must detect each other and agree on a speed — Low, Full, or High. It's a
> repeater sitting between host and device, running that negotiation on both
> ports: connect detection, the USB reset, and the High-Speed 'chirp'
> handshake. It only touches the electrical line state, never data bytes."

---

## Quick Verilog↔VHDL translation (so the syntax stops being scary)

| Verilog | VHDL equivalent | Meaning |
|---|---|---|
| `module ... endmodule` | `entity` + `architecture` | the module |
| `input`/`output [1:0] x` | `IN`/`OUT ...(1 downto 0)` | ports |
| `reg` | `signal` (that's assigned in a process) | holds a value |
| `wire` / `assign` | concurrent `signal <=` | continuous connection |
| `always @(posedge clk)` | `process(clk)` + `rising_edge(clk)` | clocked block |
| `<=` | `<=` | (same) non-blocking assignment |
| `parameter X = 5'd0` | `constant X : ... := "00000"` | named constant |
| `case ... endcase` | `case ... end case` | (same idea) |
| `5'd0` | decimal 0 in 5 bits | sized literal |

With that table, everything below reads like the VHDL state machines you
already know.

---

## Ports (lines 25–90): a two-port repeater

```verilog
input  clk_1x, clk_1x_rst_n;              // one clock + active-low reset for BOTH ports
input  [1:0] us_linestate, ds_linestate;  // the electrical state of each port's wires
output [1:0] us_xcvrselect, ds_xcvrselect; // "which transceiver mode" per port
output us_termselect, ds_termselect;       // termination resistor control
output [1:0] us_opmode, ds_opmode;         // operating mode per port
...
output clk_hs_enable, clk_fs_enable, clk_ls_enable;  // tell the rest of the chip the speed
output [1:0] ds_speed;
```
- **`us_` = upstream (toward host), `ds_` = downstream (toward device).** Two of
  everything — this is a repeater in the middle.
- **`linestate[1:0]`** is the only *input* about the wires: it encodes the two
  USB data lines (D+/D−) as 2 bits. The four values mean J, K, SE0 (both low),
  or SE1. The whole module is driven by watching this.
- **`xcvrselect`/`termselect`/`opmode`** are **UTMI** control outputs — they tell
  the physical PHY chip "switch to HS mode," "enable/disable the termination
  resistors," etc. This module doesn't drive the wires directly; it commands the
  PHY to.
- **`clk_hs/fs/ls_enable`** — once a speed is agreed, these tell the rest of your
  design which speed to clock the datapath at.

### Static assignments (lines 231–235)
```verilog
assign us_suspendm = 1'b1;    assign ds_suspendm = 1'b1;
assign ds_dppulldown = 1'b1;  assign ds_dmpulldown = 1'b1;
assign ds_drvvbus = 1'b1;
```
- Tied high permanently: never suspend, always enable the downstream pulldown
  resistors (host-side behavior — the repeater acts as a host toward the
  device), always drive VBUS (supply power downstream). These being constants
  tells you the design doesn't implement suspend/low-power on this path.

---

## The upstream state machine (lines 239–...): connect → reset → chirp → settled

Structure is exactly the Phase-1 pattern in Verilog: reset branch, then a big
`case`.

### Reset branch (lines 241–255)
```verilog
if (!clk_1x_rst_n) begin
    us_termselect <= 1'b0; us_xcvrselect <= 2'b0; us_opmode <= 2'b00;
    us_k_count <= 3'b000; us_j_count <= 3'b000;   // chirp counters (see below)
    us_rsrw_state <= US_IDLE;
    ...
```
- Note this is `always @(posedge clk_1x or negedge clk_1x_rst_n)` — the reset is
  in the sensitivity list, so unlike the *synchronous* resets in the VHDL files,
  this one is **asynchronous** (takes effect immediately, not on the next edge).
  A real difference worth naming if asked.

### `US_IDLE` (lines 259–270) — wait for the other side to see a connect
```verilog
US_IDLE : begin
    us_termselect <= 1'b0; us_xcvrselect <= 2'b0; us_opmode <= 2'b00;
    if (ds_rsrw_state == DS_HSFS_CONNECT)  us_rsrw_state <= US_HSFS_CONNECT;
    else if (ds_rsrw_state == DS_LS_CONNECT) us_rsrw_state <= US_LS_CONNECT;
    else us_rsrw_state <= US_IDLE;
```
- The upstream FSM watches the **downstream** FSM's state and follows it. This
  coupling is how a repeater keeps both ports in lockstep — when the device side
  detects a connection, the host side mirrors it.

### `US_HSFS_CONNECT` (lines 271–284) — wait for USB reset
```verilog
US_HSFS_CONNECT : begin
    us_termselect <= 1'b1; us_xcvrselect <= 2'b01;   // Full-Speed mode initially
    us_se0_cnt_enable <= 1'b1;                        // start counting SE0 time
    if (us_se0_detect) begin us_rsrw_state <= US_HSFS_RESET; end
```
- Every USB device starts in Full Speed, then may be bumped to High Speed. The
  host signals **reset** by driving **SE0** (both lines low) for a sustained
  time; `us_se0_detect`/`us_se0_cnt_enable` time that. Sustained SE0 → move to
  reset handling.

### `US_HS_CHIRPK` (lines 302–314) — the device's chirp
```verilog
US_HS_CHIRPK : begin
    us_xcvrselect <= 2'b00; us_opmode <= 2'b10;   // HS mode, raw transmit
    us_txvalid <= 1'b1; us_txactive <= 1'b1;      // drive a continuous 'K' chirp
    if (ds_rsrw_state == DS_HS_WAIT_ST) us_rsrw_state <= US_HS_WAIT_ST;
```
- During reset, a High-Speed-capable device asserts a continuous **chirp-K** to
  announce "I can do HS." `opmode = 2'b10` puts the PHY in raw-transmit mode to
  emit it.

### `US_HS_WAITFOR_CHIRPK` / `US_HS_WAITFOR_CHIRPJ` (lines 332–400) — the handshake count
```verilog
US_HS_WAITFOR_CHIRPK : begin
    if ((us_k_count >= 3'd4) && (us_j_count >= 3'd4) && us_se0_detect) begin
        us_k_count <= 3'd0; us_j_count <= 3'd0;
        us_rsrw_state <= US_HS_END_OF_RESET;         // enough K-J pairs → HS confirmed
    end
    else if (us_detect_k) begin
        if (us_k_count < 3'd4) us_k_count <= us_k_count + 1;
        us_rsrw_state <= US_HS_WAITFOR_CHIRPJ;       // saw a K, now wait for a J
    end
    else if (us_TWTFS == 2'b11) us_rsrw_state <= US_FS_END_OF_RESET;  // timed out → fall back to FS
```
- **This is the core of High-Speed negotiation, and the best thing to explain in
  an interview.** After the device chirps K, the host replies with an
  alternating **K-J-K-J...** pattern. Both sides count the alternations
  (`us_k_count`, `us_j_count`). The USB spec requires seeing a minimum number of
  K-J pairs to confirm HS; this code waits for **≥4 of each** (`>= 3'd4`).
  - See enough K-J pairs → **`US_HS_END_OF_RESET`** = High Speed agreed.
  - Timer expires first (`us_TWTFS == 2'b11`) → **`US_FS_END_OF_RESET`** = fall
    back to Full Speed.
- **Interview line:** "High-Speed detection is a chirp handshake: the device
  chirps K, the host answers with alternating K-J bursts, both count them, and
  if enough pairs are exchanged before a timeout, both switch to High Speed;
  otherwise they stay Full Speed. The FSM counts to 4 K's and 4 J's."
- The `` `ifdef SIM_ONLY `` / `` `else `` blocks (lines 355–359, etc.) swap the
  timeout condition between simulation and real hardware — in sim it uses a fast
  1µs tick, on hardware a real timer. Good detail: shows the module was written
  to be simulatable without waiting real USB timescales.

### After this
- **`US_HS_SETTLED` / `US_FS_SETTLED` / `US_LS_SETTLED`** are the terminal
  "we're running at speed X" states. Reaching one asserts the matching
  `clk_hs/fs/ls_enable` so the rest of your design knows the negotiated speed.
- There's a parallel **`ds_rsrw_state`** machine (the DS_* states) doing the
  mirror-image job on the device-facing port. Same shape, so once you can
  explain the US side you can explain the DS side.

---

## Where this module sits in the whole project (tie it together)

- Phase 5 is the **bottom** of the stack — it runs *first*, at power-on, getting
  the link up and agreeing on a speed.
- Only *after* a `SETTLED` state do the byte-level pipelines (Phases 1–4) have
  valid data to process.
- It shares **no data signals** with the content pipeline — only the
  `clk_*_enable`/`ds_speed` outputs, which tell the datapath what speed to
  expect. That's why, in earlier tracing, it looked "disconnected": it connects
  through the PHY wrappers (`sie_link_wrapper`) and clock-enables, not through
  the rxdata bus.

## New concepts learned this phase
1. **Reading Verilog** via the translation table.
2. **Async reset** (`negedge rst_n` in the sensitivity list) vs the VHDL sync resets.
3. **Line state (J/K/SE0)** — USB's electrical signaling, 2 bits.
4. **UTMI control** (`xcvrselect`/`termselect`/`opmode`) — commanding a PHY.
5. **The chirp handshake** and K-J pair counting for HS negotiation.
6. **`` `ifdef SIM_ONLY ``** — sim-vs-hardware conditional compilation.
7. **Coupled FSMs** — two state machines watching each other (repeater lockstep).
8. **Recognizing vendor IP** by its copyright header.

---

### Next: Phase 6 (final) — `fpga_usb_dev.vhd` + `usb2_hs_pie_process.vhd`:
the integration layer and the top-down narrative that ties all six phases into
the "walk me through your project" answer.

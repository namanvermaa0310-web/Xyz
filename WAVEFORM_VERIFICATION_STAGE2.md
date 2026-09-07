# STAGE 2 — Waveform Verification
### Verify the loopback datapath from the wave window. No `$display` needed.

## Setup

```tcl
vlog ftile_eth_400g_model.v eth400g_loopback.v tb_loopback.v
vsim tb_loopback
```

```tcl
add wave -divider "CONTROL"
add wave /tb_loopback/clk
add wave /tb_loopback/rst_n
add wave /tb_loopback/gen_enable
add wave -radix unsigned /tb_loopback/force_len
add wave -radix unsigned /tb_loopback/stall_rate

add wave -divider "RX  (model -> loopback)"
add wave /tb_loopback/rx_valid
add wave -radix hex /tb_loopback/rx_inframe
add wave -radix hex /tb_loopback/rx_data
add wave -radix hex /tb_loopback/rx_eop_empty

add wave -divider "TX  (loopback -> model)"
add wave /tb_loopback/tx_ready
add wave /tb_loopback/tx_valid
add wave -radix hex /tb_loopback/tx_inframe
add wave -radix hex /tb_loopback/tx_data
add wave -radix hex /tb_loopback/tx_eop_empty

add wave -divider "LOOPBACK INTERNALS"
add wave /tb_loopback/u_lb/tx_en
add wave -radix hex /tb_loopback/u_lb/rdy_pipe
add wave -radix unsigned /tb_loopback/u_lb/wr_ptr
add wave -radix unsigned /tb_loopback/u_lb/rd_ptr
add wave -radix unsigned /tb_loopback/fifo_level
add wave /tb_loopback/u_lb/full
add wave /tb_loopback/u_lb/empty
add wave /tb_loopback/u_lb/rd_vld_d

add wave -divider "RESULTS"
add wave -radix unsigned /tb_loopback/rx_beats
add wave -radix unsigned /tb_loopback/tx_beats
add wave -radix unsigned /tb_loopback/ovf_beats
add wave -radix unsigned /tb_loopback/mismatch
add wave -radix unsigned /tb_loopback/m_viol

run -all
```

---

## CHECK 1 — Data integrity (the headline)

| Signal | Expect |
|---|---|
| `mismatch` | **stays 0 for the entire run** |

This is the whole point of stage 2. It compares every beat leaving the
loopback against the beat that entered, so any corruption, drop, duplication
or reordering increments it.

Right-click `mismatch` → **Insert Breakpoint** with condition `mismatch != 0`
if you want the simulator to stop the instant it ever moves.

---

## CHECK 2 — Data actually flows through, delayed

Pick any beat during phase A. Note `rx_data` at time T.
The same value must appear on `tx_data` a few cycles later.

| Expect | |
|---|---|
| `tx_data` matches an earlier `rx_data` | yes |
| Delay | small and constant while the FIFO level is steady |
| `rx_beats` vs `tx_beats` | track each other, differing only by what is in flight |

Use **Edit → Find** on `tx_data` for a distinctive `rx_data` value to confirm
the same bits came out.

---

## CHECK 3 — FIFO behaves

| Signal | Expect during phase A | Meaning |
|---|---|---|
| `wr_ptr` / `rd_ptr` | both incrementing, small constant gap | balanced flow |
| `fifo_level` | small and stable | RX and TX rates match |
| `full` | **0** | never fills at line rate without stalls |
| `empty` | occasionally 1 at start | normal before the first beat lands |
| `ovf_beats` | **0** | nothing dropped |

If `fifo_level` climbs steadily during phase A, TX is not keeping up and
something is wrong.

---

## CHECK 4 — TX PAUSE PROTOCOL under real load (phase D)

**This is the screenshot to take for stage 2.**

Phase D sets `stall_rate = 28` hex (40 decimal). Zoom on any `tx_ready` fall.

| Expect | |
|---|---|
| Cycles from `tx_ready` fall to `tx_valid` fall | exactly **3** (`READY_LATENCY`) |
| `tx_data` and `tx_inframe` while `tx_valid` low | **frozen** — no transitions at all |
| `rdy_pipe` | `tx_ready` shifting through, 3 stages |
| `tx_en` | equals `rdy_pipe[1]` |
| `rd_ptr` | **stops advancing** while `tx_en` is low |
| `m_viol` | **0** throughout |

The difference from stage 1: here the datapath is actually *carrying data*
when the stall hits, so this proves the freeze works on live traffic rather
than on idle beats.

**The trap:** a conventional ready/valid handshake would drop `tx_valid` in
the same cycle as `tx_ready`. If you see that, the protocol is wrong.

---

## CHECK 5 — Overflow is graceful, not corrupting (phase D)

Phase D deliberately overflows: RX runs at line rate, TX stalls ~40% of the
time, and there is no flow-control path back to the source.

| Signal | Expect | Meaning |
|---|---|---|
| `ovf_beats` | rises to roughly **1490** | beats legitimately dropped |
| `full` | asserts periodically | FIFO genuinely fills |
| `mismatch` | **still 0** | **every beat that DOES get through is bit-exact** |

That last row is the point. Dropping under sustained backpressure is a
property of the link (UG sec 7.5: *"The interface does not take direct
backpressure"*), not a defect. Corrupting is a defect. This shows drops
without corruption.

The production fix is PAUSE/PFC flow control (UG sec 4.2.3), which stops the
far end instead of dropping — that belongs in a later stage.

---

## CHECK 6 — Multi-frame-per-beat survives the datapath (phase B)

Phase B forces 64-byte frames, so two frames occupy every beat.

Expand `rx_data` on a beat where `rx_inframe = ffff`. You will see the
broadcast destination address `FFFFFFFFFFFF` **twice**, 512 bits apart:

| Bit range | Content |
|---|---|
| `rx_data[47:0]` | frame A destination address |
| `rx_data[559:512]` | frame B destination address |

Now find the corresponding `tx_data` a few cycles later. **Both frames must
still be there, in the same positions.** That is the proof the beat-level
datapath preserves multi-frame beats intact.

Cross-check: `m_rx_frames` reaches **40000** over 20000 beats — exactly 2.0
frames per beat, through the datapath.

---

## CHECK 7 — Error sideband passes through (phase E)

| Expect | |
|---|---|
| `rx_error` non-zero on EOP segments | model is injecting |
| `tx_error` non-zero on the same segments a few cycles later | sideband carried |
| `mismatch` | still 0 |

The loopback flattens the 2-bit-per-segment RX error code into the 1-bit-per-
segment TX error, per UG Tables 43/46. Confirm the bit lands in the *same*
segment position.

---

## Two screenshots that summarise stage 2

1. **Phase D zoomed on a `tx_ready` fall** — cursors showing exactly 3 cycles
   to `tx_valid` falling, `tx_data` frozen, `rd_ptr` stopped, `mismatch` = 0.
2. **Phase D wide view** — `ovf_beats` climbing to ~1490 while `mismatch`
   stays flat at 0. Drops without corruption.

---

## What stage 2 does NOT prove

No reset or status sequencing. No processing stage (that is stage 3). No PMA,
PCS, RS-FEC, AN/LT or link training. The model stands in for the IP; it does
not validate it.

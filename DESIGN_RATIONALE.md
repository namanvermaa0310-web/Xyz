# File-by-File Design Rationale
### Every decision, and the clause in UG 683023 that forced it

Four files. Two are your product, two are test infrastructure. This document explains what each
one does, why it is built that way, and which part of the IP documentation drove the choice.

```
   ftile_eth_400g_model.v   ── test infrastructure, deleted when IP arrives
            │  RX                                   ▲  TX
            ▼                                       │
   ┌────────────────────────────────────────────────────────┐
   │  eth400g_loopback.v          ← YOUR DESIGN             │
   │     ├── pipe_proc.v          ← YOUR DESIGN             │
   │     └── beat FIFO                                      │
   └────────────────────────────────────────────────────────┘

   tb_eth400g_top.v         ── test infrastructure
```

---

# FILE 1 — `eth400g_loopback.v` (the design)

## What it does

Takes beats from the RX MAC segmented client interface, pushes them through a pipelined
processing stage and an elastic FIFO, and drives them back out on the TX MAC segmented client
interface.

## Decision 1 — There is no packet layer. At all.

**What we did:** the module never inspects `inframe`, never detects SOP or EOP, never counts
packets. It buffers 1024-bit beats and replays them, carrying `inframe` / `eop_empty` / `error` /
`skip_crc` through as opaque sideband.

**Why — UG §7.5:**
> *"Packets may start on any 8-byte segment... For multisegmented interfaces, a new packet may
> start and the previous packet end are within the same cycle."*

**Why — UG §7.4 (Attention box):**
> *"To achieve the maximum throughput when using the TX MAC segmented interface, the input
> packets need to be packed tightly, leaving no idle segments in between."*

**The arithmetic that makes this unavoidable:** a beat is 1024 bits = 128 bytes. The minimum
Ethernet frame is 64 bytes. So **two complete frames land in one clock cycle routinely** — not as
a corner case.

**What breaks without this decision:** the obvious implementation keeps one packet state per beat
— a single `sop_ptr`, a single `in_packet` flag, `packet_count++` on `any_eop`. With two frames
in a beat that counts one, snapshots one pointer, and eventually commits truncated frames. It
passes light testing and corrupts traffic under real load.

**Why this is better than "handling" it:** handling multi-frame-per-beat with packet bookkeeping
means 16 parallel packet state machines. For a loopback that buys nothing — what goes out is
exactly what came in. Removing the packet layer makes the failure mode **structurally impossible**
rather than merely guarded against.

## Decision 2 — TX implements a fixed-latency pause protocol, not ready/valid

**This is the single most important line of code in the project.**

```verilog
wire tx_en = (READY_LATENCY >= 2) ? rdy_pipe[READY_LATENCY-2] : i_tx_mac_ready;
...
o_tx_mac_valid <= tx_en;      // valid is READY delayed, never data-driven
```

**Why — UG §7.4, verbatim:**
> - *"The `i_tx_mac_valid` signal deasserts when the `o_tx_mac_ready` signal is deasserted."*
> - *"The `i_tx_mac_valid` signal asserts only when the `o_tx_mac_ready` signal is asserted,
>   **even though there is no packet to send**."*
> - *"The `i_tx_mac_valid` and the `o_tx_mac_ready` signals can be spaced by a **fixed latency
>   between 1 to 7 clock cycles**."*
> - *"When `i_tx_mac_valid` deasserts, `i_tx_mac_data`, `i_tx_mac_inframe`, `i_tx_mac_eop_empty`,
>   `i_tx_mac_error` and `i_tx_skip_crc` signals **must be paused for as many cycles as
>   `o_tx_mac_ready` is deasserted**."*

**Three consequences most people get wrong:**

1. **`valid` is not derived from whether you have data.** It is `ready` delayed by exactly
   `READY_LATENCY`. This is the opposite of AXI.
2. **An idle beat is `inframe = 0`, not `valid = 0`.** With nothing to send you still assert
   valid and drive all-zero inframe:
   ```verilog
   end else begin
       o_tx_mac_data    <= 0;
       o_tx_mac_inframe <= 0;   // idle beat - valid STAYS asserted
   end
   ```
3. **The whole datapath freezes when ready is low** — including the processing pipeline, via
   `pipe_proc`'s `i_en` gate.

**Why `READY_LATENCY-2` and not `-1`:** the outputs are registered, which adds one cycle.
Tapping the delay pipe one stage earlier makes the registered `o_tx_mac_valid` land at exactly
`READY_LATENCY` cycles after `o_tx_mac_ready`. This was a real off-by-one bug caught by the
protocol checker — it produced 4601 violations in the stall phase before it was fixed.

**What a conventional handshake would have done:** `out_free = ~valid | ready` compiles, passes a
naive testbench, and fails against the real IP. It is the wrong protocol, not a suboptimal one.

## Decision 3 — RX error is 32 bits, TX error is 16 bits, so they must be flattened

```verilog
assign rx_err_flat[gi] = (|i_rx_mac_error[gi*2 +: 2]) | i_rx_mac_fcs_error[gi];
```

**Why — UG Table 46 vs Table 43:** `o_rx_mac_error` is **2 bits per segment** (32 bits total),
encoding four states: `0` no error, `1` malformed, `2` under/oversized, `3` payload length error.
`i_tx_mac_error` is **1 bit per segment** (16 bits total).

Any non-zero RX error code, or an FCS error, collapses to the single TX error bit.

**Why this matters later:** for a real encryptor you probably do not want to loop errored frames
back at all — you want to drop them. That is a policy decision that belongs in `pipe_proc`, not
here.

## Decision 4 — Only real data beats consume FIFO space

```verilog
end else if (p_valid && (|p_inframe)) begin
```

**Why:** `o_rx_mac_valid` qualifies the whole interface cycle; `o_rx_mac_inframe` says which
segments contain frame data. They mean different things. An RX beat with `valid=1` but
`inframe=0` carries no frame content and must not occupy buffer space. **Both conditions are
required** — one is not a substitute for the other.

## Decision 5 — Overflow is counted, never silent

**Why — UG §7.5:** *"The interface does not take direct backpressure."*

RX cannot be stalled. If the FIFO is full the beat is lost — that is physics, not a defect, since
there is no flow-control path back to the source. What matters is that it is **counted**
(`o_ovf_beats`) so it can never be silent.

**The proper production fix is in the IP, not here.** UG §4.2.3 describes PAUSE/PFC flow control
with a dedicated pause generation interface. Wire that to FIFO occupancy in a real encryptor —
dropping frames also breaks the MACsec replay window at the far end.

---

# FILE 2 — `pipe_proc.v` (the design)

## What it does

A fully pipelined per-segment transform. Placeholder for AES-GCM / SecY.

## Decision 1 — Initiation interval = 1, no internal stall path

**Why:** this is what makes 400G possible. One 1024-bit beat in and one out **every enabled
cycle**. Latency is `PIPE_STAGES` cycles and is irrelevant to throughput.

```
1024 bits/cycle × 415.0390625 MHz = 425 Gbps
```

Four registered stages, all independent:
- **Stage 0** capture
- **Stage 1** 16 parallel 32×32 multiplies (one per segment, infers DSP blocks)
- **Stage 2** add round constant
- **Stage 3** XOR fold + output register

**Why real multiplies and not a wire:** so DSP inference and timing are representative of what
AES-GCM will actually cost. A pass-through would tell you nothing about whether 415 MHz closes.

## Decision 2 — Every register is gated by `i_en`

```verilog
end else if (i_en) begin
    s1_mul[m] <= s0[m*SEG_W +: 32] * s0[m*SEG_W+32 +: 32];
```

**Why — UG §7.4:** *"all TX signals must be paused for as many cycles as `o_tx_mac_ready` is
deasserted."* Gating the pipeline is how that freeze propagates backwards without losing data.
A pipeline that keeps advancing during a TX pause drops beats.

## Decision 3 — Sideband travels a matched delay line

`inframe` / `eop_empty` / `error` / `skip_crc` go through a `PIPE_STAGES`-deep shift register so
they stay bit-aligned with the data they describe.

**Why it is frame-agnostic:** this module never inspects packet boundaries. That is what makes it
immune to multiple frames per beat, exactly like the parent.

## The contract for replacing this with crypto

Keep two properties and nothing around it changes:
1. **II = 1**, no internal stall
2. **Every register gated by `i_en`**

---

# FILE 3 — `ftile_eth_400g_model.v` (test infrastructure)

## What it does

Two jobs: generate realistic RX traffic, and **police the TX protocol**.

## Decision 1 — Frames are packed tightly with no gap segments

```verilog
rx_active = 1'b0;   // next segment may start a NEW frame immediately
rx_off    = 0;
```

**Why — UG §7.4:** tight packing is mandated. An earlier version inserted one idle segment
between frames and measured 370 Gbps instead of ~400 — the gap was costing throughput. More
importantly, without tight packing the model **never produces the multi-frame-per-beat case**,
which is exactly the condition the design must survive.

## Decision 2 — Correct signal widths, including the ones easily missed

| Signal | Width | Source |
|---|---|---|
| `o_rx_mac_error` | **32** (2/segment) | UG Table 46 |
| `i_tx_mac_error` | **16** (1/segment) | UG Table 43 |
| `o_rx_mac_status_data` | 48 (3/segment) | UG Table 46 |
| `i_tx_mac_skip_crc` | 16 (1/segment) | UG Table 43 |

The last two were missing from an earlier version entirely.

## Decision 3 — It enforces the protocol, it does not just drive signals

Two active checks:

```verilog
// Rule 1: valid must equal ready delayed by exactly READY_LATENCY
if (i_tx_mac_valid !== exp_valid) ...

// Rule 2: the bus must be FROZEN while valid is low
if (!i_tx_mac_valid && have_hold) begin
    if ((i_tx_mac_data !== tx_d_hold) || (i_tx_mac_inframe !== tx_if_hold)) ...
```

**Why:** a model that only supplies stimulus lets protocol bugs through silently. This one caught
the `READY_LATENCY` off-by-one. Without it that bug ships and fails on hardware.

## Decision 4 — TX frame counting is deliberately NOT implemented

**Why:** UG §7.4 says EOP is an `inframe` transition from 1 to 0 between consecutive segments.
But §7.4 also mandates tight packing with no idle segments — under which `inframe` stays high
across a frame boundary and **no such transition exists**. §7.5 has the same contradiction and
additionally refers to an *"`o_rx_mac_eop_empty` transition from 1 to 0"*, which does not
typecheck against `eop_empty` being a 3-bit-per-segment count.

**Both statements cannot be literally true.** Writing a counter now would encode a guess. The
testbench verifies beat-exact data equality instead, which is a **strictly stronger** check than
a frame count.

Resolve from the generated design example before writing SecTAG/ICV logic.

---

# FILE 4 — `tb_eth400g_top.v` (test infrastructure)

## Decision 1 — Clock is 415.0390625 MHz

```verilog
localparam CLK_HALF = 1204.75;   // ps
```

**Why — UG §5:** `i_clk_tx` and `i_clk_rx` are driven by `o_clk_pll`, and `o_clk_pll` is
*"415.0390625 MHz or higher for all Ethernet modes with IEEE 802.3 RS(544,514) (CL134)"* —
RS(544,514) is KP4 FEC, i.e. 400GE.

**Why not 390.625 MHz**, which gives the tidy 1024 × 390.625 = 400 Gbps: that figure is
`o_clk_tx_div`, described as *"Clock recovered from the TX SERDES rate divided by either
33/66/68"* — a SERDES-derived divided clock for TOD/PTP. **Different clock.**

**Why the interface runs faster than the payload rate:** 1024 × 415.0390625 = **425 Gbps of
interface capacity**, carrying 400 Gbps of payload plus idle segments for IFG and preamble. The
headroom is what makes sustained line rate deliverable, not slack to be optimised away.

## Decision 2 — Reference queue hooks the DUT's actual FIFO write

```verilog
if (u_lb.p_valid && (|u_lb.p_inframe) && !u_lb.full) begin
    refq[qw % QD] <= {u_lb.p_data, u_lb.p_inframe, u_lb.p_eop_empty};
```

**Why:** an earlier version enqueued on the RX **input** event while the DUT writes on the
**pipeline output** event four cycles later. Under overflow the two conditions disagree, the
queue desynchronises by one entry, and every subsequent compare mismatches — turning 978 dropped
beats into 2920 false "mismatches". Hooking the real write event fixed it.

## Decision 3 — Four phases, each testing something different

| Phase | Tests |
|---|---|
| 1 — mixed-length, no stall | Baseline correctness and throughput |
| 2 — 64-byte frames | **Multi-frame-per-beat.** The condition that breaks naive designs |
| 3 — TX stalls | Pause-protocol compliance and freeze behaviour |
| 4 — RX error injection | Sideband integrity through the pipeline |

## Decision 4 — Per-phase reset

Cumulative counters across phases make it impossible to tell which phase a failure came from.
Each phase resets and reports independently.

---

# RESULTS

| Phase | Interface rate | Packet rate | Mismatches | Overflow | Protocol violations |
|---|---|---|---|---|---|
| 1 mixed-length | 369.5 Gbps | 3.15 Mpps | 0 | 0 | 0 |
| 2 64-byte frames | 369.5 Gbps | 45.1 Mpps | 0 | 0 | 0 |
| 3 TX stalls | 351.4 Gbps | 62.96 Mpps | 0 | 978 | 0 |
| 4 RX errors | 327.2 Gbps | 54.96 Mpps | 0 | 0 | 0 |

Phase 3 overflow is expected — RX takes no backpressure and there is no flow control path, so
sustained stalling must eventually overflow. Every beat that gets through is bit-exact.

---

# BUGS FOUND AND FIXED DURING DEVELOPMENT

Worth citing — each one would have shipped:

1. **Wrong TX protocol.** Used ready/valid instead of fixed-latency pause. Would have failed
   against the real IP despite passing a naive testbench.
2. **`READY_LATENCY` off-by-one.** Registered outputs added a cycle; valid landed at
   `READY_LATENCY+1`. 4601 protocol violations, caught by the model's checker.
3. **Mid-packet FIFO overflow committing truncated frames** (earlier packet-level version).
   Removed entirely by dropping the packet layer.
4. **Multiple drivers on a packet counter** (earlier version) — incremented in one always block,
   decremented in another. Illegal Verilog.
5. **Registered-output handshake violation** (earlier version) — presenting a beat while ready
   was low and advancing anyway. Showed as 30 sent, 18 received.
6. **Inter-frame gap inserted mid-frame** in the model, chopping frames in half.
7. **Reference queue desynchronisation** under overflow, producing false mismatches.

---

# WHAT IS NOT MODELLED

State this alongside any result:

Reset sequencing (12-step, six signals) · all status/link-up signals · PMA · PCS ·
RS-FEC (KP4) · lane distribution · gearbox · CDR · alignment marker lock · AN/LT ·
PAUSE/PFC flow control · `skip_crc` semantics (SA insertion, padding, CRC substitution) ·
statistics registers and CSR/Avalon-MM · PTP/TOD · preamble passthrough · MTU enforcement

**This validates the fabric datapath against a model of the MAC segmented client interface —
not the complete IP.**

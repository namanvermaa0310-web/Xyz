# Complete Pin Mapping Guide — Silicon Architecture Through to Schematic

This combines two things: the general 8-category pin-wiring rules from before, and the internal
F-Tile silicon structure (PMAs, streams, fractures) from the F-Tile Architecture and PMA/FEC
Direct PHY IP User Guide. **The reason to combine them:** for GPIO, you can assign pins fairly
freely. For F-Tile transceiver pins, you cannot — the silicon has a fixed internal resource
structure, and your pin map has to follow it, not the other way around. This guide shows you that
structure so your pin assignments are grounded in what's actually there, not guesswork.

---

## Part 0 — Why F-Tile pins are a different kind of problem

A GPIO ball is a GPIO ball — any one in the right bank will do. A transceiver ball is the
endpoint of a fixed internal signal path: **physical lane → PMA → stream(s) → EMIB → fracture →
fabric.** Every link in that chain has a limited quantity. If you don't map pins against this
chain, you'll assign something Quartus will simply reject — or worse, something it accepts but
that silently limits you later (e.g. burning your only st_x16 fracture on the wrong port).

```
   Physical      PMA          Stream(s)      EMIB        Fracture         Fabric
   diff pair  →  (FHT/FGT)  →  (1-4 per   →  (physical →  (MAC+PCS+FEC  →  (your
   on the ball                  PMA,          crossing)    hard block,      RTL)
                                 by width)                  sized st_x1
                                                             .. st_x16)
```

Everything in Part 2 walks this chain, left to right.

---

## Part 1 — Quick recap of the general workflow (full detail in the previous guide)

1. Pick exact device in Quartus.
2. Instantiate IP first (this reserves transceiver/clock pins automatically).
3. Assign remaining pins in Pin Planner, one voltage per bank.
4. Run I/O Assignment Analysis to catch conflicts.
5. Export the pin file (signal → ball → bank → voltage).
6. Script the pin file into a multi-part OrCAD symbol (never hand-type it).
7. One schematic sheet per part — power, SDM, per-bank GPIO, per-bank transceiver.
8. Wire each pin per its category (power, ground, config, clock, high-speed, GPIO, unused).

Part 2 below is the missing piece specifically for step 2/3 on transceiver pins — you can't
intelligently instantiate the Ethernet IP or assign its pins without knowing this.

---

## Part 2 — Inside the F-Tile: the structure that decides your transceiver pin map

### 2.1 Two families of physical lane (PMA)

Every F-Tile has two separate pools of SerDes lanes, and they are **not interchangeable**:

```
   +-------------------------- ONE F-TILE --------------------------+
   |                                                                  |
   |   FHT PMA group                    FGT PMA group                |
   |   (up to 4 lanes)                  (up to 16 lanes)              |
   |   fastest: up to 116 Gbps          fastest: up to 58 Gbps        |
   |   used for: single-lane 100G,      used for: 10G/25G/50G/100G-   |
   |             400GbE-4 (4x106G)      class links, 400GbE-8 (8x53G) |
   +-------------------------------------------------------------------+
```

**Practical effect on pin mapping:** before you assign a single pin, decide whether your port
needs FHT lanes (fewer, faster — e.g. 400GbE-4) or FGT lanes (more, slower per lane — e.g.
400GbE-8, or your 100G port). That decision determines which physical balls on the package you're
even allowed to pick from.

### 2.2 Streams — the real resource budget, not the lane count

Each PMA doesn't map 1-to-1 onto fabric bandwidth. It maps to a number of **streams**, and the
number depends on the PMA's *width*, not just its existence:

```
   PMA width       Streams consumed      Example
   ------------------------------------------------------------
   32-bit      →    1 stream        →    lower-rate NRZ lanes
   64-bit      →    2 streams       →    53.125 Gbps PAM4 lane (used in 400GbE-8)
   128-bit     →    4 streams       →    106.25 Gbps PAM4 lane (used in 400GbE-4)
```

A tile has **16 streams total**, full stop. Both 400G modes use all 16, just via different lane
counts:

```
   400GbE-8:  8 PMAs × 2 streams each  = 16 streams  (8 physical diff pairs used)
   400GbE-4:  4 PMAs × 4 streams each  = 16 streams  (4 physical diff pairs used)
```

**Practical effect on pin mapping:** streams, not raw lane count, are what run out. Two ports
that each "only" use 4 lanes can still be impossible to fit together if their combined stream
count exceeds 16 — check streams, not just how many diff pairs look free.

### 2.3 Fractures — how streams become an actual MAC+PCS+FEC port

A **fracture** is a chunk of the hard IP (MAC + PCS + FEC logic) sized to a specific stream count.
The tile has a *fixed inventory* of fracture sizes — you don't get to invent new combinations:

```
   Fracture   Streams/fracture   How many fit in the tile   Typical rate
   ------------------------------------------------------------------------
   st_x1            1                    16                 10G / 25G
   st_x2             2                     8                 50G
   st_x4             4                     4                 100G  (100GbE-1/2/4)
   st_x8             8                     2                 200G
   st_x16           16                     1                 400G  (400GbE-4/8)
```

**This is the single most important number for your project:** only **one** st_x16 fracture
exists per tile — meaning one 400G port per tile, no matter how you slice the lanes. But **four**
st_x4 fractures exist — meaning up to four independent 100G-class ports per tile. This is exactly
the math from earlier in this project; it's grounded in this table.

**Fractures share the same underlying streams, so using a big one blocks the smaller ones that
overlap it.** If you build one st_x16 port, every st_x8/st_x4/st_x2/st_x1 fracture in that tile
becomes unavailable — there's nothing left. Plan which fracture size you're committing to before
you assign a single pin.

### 2.4 Worked example: mapping a single 100GbE-4 port, pin by pin

This is the exact sequence you'll repeat for your MACsec port.

**Step A — pick the fracture.** 100GbE-4 needs one st_x4 fracture (4 streams).

**Step B — pick the lane mode.** 100GbE-4 uses 4 lanes at 25.78 Gbps NRZ — a 32-bit-wide PMA
config, 1 stream each, 4 lanes × 1 stream = 4 streams. That's FGT-class lanes (NRZ, lower rate).

**Step C — pick which physical quad.** Reference clocks are wired per-quad (4 lanes sharing one
reference clock pair) — confirmed directly from the dev kit schematic. So your 4 lanes need to be
one physical quad on your chosen bank, not 4 lanes borrowed from different quads.

**Step D — this is what actually lands in your pin file:**

```
   Signal name                Ball        Bank     Notes
   ------------------------------------------------------
   FGT12C_TX_Q0_CH0_P/N       (per device)  12C    Lane 0, quad 0
   FGT12C_TX_Q0_CH1_P/N       (per device)  12C    Lane 1, quad 0
   FGT12C_TX_Q0_CH2_P/N       (per device)  12C    Lane 2, quad 0
   FGT12C_TX_Q0_CH3_P/N       (per device)  12C    Lane 3, quad 0
   FGT12C_RX_Q0_CH0_P/N       (per device)  12C    (+3 more RX lanes)
   REFCLK_FGT12C_Q0_P/N       (per device)  12C    ONE reference clock pair, whole quad
```

**Step E — schematic and board.** Those TX/RX pairs go straight to your QSFP/QSFP28 cage
(no AC coupling caps needed — module and on-die termination handle it, per the earlier dev-kit
walkthrough). The reference clock pair goes to your oscillator per §D of the previous guide.

```
   Oscillator ---- REFCLK_FGT12C_Q0_P/N ----→ FPGA quad 0 reference clock input
                                                      |
                                          (feeds System PLL, see 2.5)
                                                      |
   Cage TX0..3 P/N  ←---- FPGA FGT12C_TX_Q0_CH0..3 P/N
   Cage RX0..3 P/N  ----→ FPGA FGT12C_RX_Q0_CH0..3 P/N
```

That's the whole chain, concretely, for one port — this is what "pin mapping" actually means for
a transceiver interface: not picking convenient-looking pins, but walking PMA → stream →
fracture → quad → physical ball in order.

### 2.5 The clocking block that must exist before anything else works

Every F-Tile design needs exactly one instance of a specific IP: the **F-Tile Reference and
System PLL Clocks IP.** It can't be compiled standalone — it has to connect to your protocol IP
(the Ethernet Hard IP) — but it's the thing that actually distributes clocks to every fracture
you build.

```
   Up to 8 physical reference clock          +----------------------------+
   input pins on the package        -------→ | F-Tile Reference & System  |
   (10 logical sources, 8 usable              | PLL Clocks IP (1 per tile) |
   for the System PLL)                        |                            |
                                               |  Up to 3 System PLLs       |
                                               |  Up to 2 FHT common PLLs   |
                                               +--------------+-------------+
                                                              |
                                        distributes clock to every fracture
                                        (your 400GbE port, your 100GbE ports,
                                         anything else on this tile)
```

**Key practical fact, taken directly from the guide:** multiple ports *can* share one System PLL
even at different rates — the fastest port on that PLL sets its frequency, and slower ports get
internally geared down. A real example from the documentation: a single System PLL running at
**830.07 MHz** serves every Ethernet speed from 10GE to 400GE simultaneously, with the
fabric-facing client clock fixed at **415.03 MHz** regardless of which rate is actually running.

**Practical effect on pin mapping:** you don't need one reference clock oscillator per port. If
your ports can share a System PLL (check the sharing rules — same tile, PLL budget not exceeded),
one clean oscillator feeding one physical reference clock pin can serve multiple ports. This
directly reduces your BOM and board complexity — fewer oscillators, fewer reference clock traces
to route with matched-length, controlled-impedance care.

### 2.6 The Avalon-MM management interface — not a board pin

One more F-Tile resource worth knowing about so you don't hunt for it on the package: **each
F-Tile has one global Avalon memory-mapped (Avalon-MM) interface** used by your fabric logic to
configure and read status from the hard IP (link status, statistics counters, dynamic
reconfiguration control). This is entirely internal — it connects your RTL to the hard IP inside
the fabric, not to a physical package pin. You will not find it in your pin file, and you don't
wire it on the schematic — it's something you instantiate and connect in Quartus/Platform
Designer, not something that shows up on your board.

---

## Part 3 — Putting Part 2 into the general pin-mapping workflow from Part 1

When you get to Step 2 ("instantiate IP first") in the general workflow, this is what you're
actually deciding:

1. **Which fracture size** does each of your ports need (§2.3)? Write this down per port before
   opening Quartus.
2. **FHT or FGT lanes** for each port (§2.1)? This depends on whether you need single-lane
   high-rate (FHT) or standard multi-lane (FGT).
3. **Which quad(s)**, and therefore which bank, hosts each port (§2.4, Step C)? This is what
   actually determines which physical balls appear in your pin file.
4. **How many reference clock oscillators do you actually need**, given PLL sharing (§2.5)? Don't
   default to one-per-port without checking whether sharing works for your rate combination.

Only after these four are answered does "assign pins in Pin Planner" become a real, informed
step rather than a guess — and only then does the schematic symbol you script in Part 1, Step 6
actually represent a design you've thought through, rather than whatever Quartus happened to
auto-place.

---

## Part 4 — Updated checklist (adds F-Tile-specific items to the earlier one)

1. Every port's fracture size is decided and documented before pin assignment starts.
2. FHT vs FGT lane choice is deliberate per port, not accidental.
3. All lanes for one port sit in the same quad; no port spans two quads.
4. Reference clock oscillator count is based on actual PLL-sharing analysis, not "one per port."
5. Total streams committed across all your ports on one tile is ≤ 16, and you've confirmed which
   fractures remain available given what you've already committed.
6. The Avalon-MM management interface is instantiated in Quartus/fabric — you have **not** looked
   for it on the package or tried to wire it on the schematic.
7. Everything from the general checklist (previous guide, Part 3) still applies on top of this.

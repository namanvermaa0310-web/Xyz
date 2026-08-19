# Pin Mapping, Explained Simply — From Quartus to Schematic to Board

Goal: take every one of the ~2000+ balls on your Agilex 7 package and know exactly what it
connects to, why, and how to draw it. This is done in stages — never jump straight to drawing
wires.

---

## Part 1 — The Workflow (in order, don't skip steps)

### Step 1: Pick your exact device in Quartus
Open Quartus, start a new project, select your exact OPN (e.g. `AGFA027R24C2I2V`). This alone
tells Quartus every legal pin on your package.

### Step 2: Tell Quartus what you're building, before touching pins
Instantiate your IP first — the Ethernet Hard IP (F-Tile or E-Tile), any PLL/clock IP, memory
interface IP if you have one. **Why first:** these IPs claim specific pins (transceiver channels,
reference clock pins, certain dedicated clock pins) automatically. If you assign pins randomly
before this, you'll fight Quartus's placement rules later.

### Step 3: Open the Pin Planner
`Assignments → Pin Planner`. This shows every pin on the package as a picture of the actual
device — you literally click a ball and tell it what signal lives there. For pins Quartus already
reserved in Step 2 (transceiver pins, refclk pins), it shows them pre-assigned — don't fight those.

For everything else (GPIO, control signals, LEDs), you choose the bank and I/O standard.
**Rule of thumb:** put all pins that need the same voltage in the same bank, because a bank has
only one VCCIO rail — you cannot mix a 1.2V signal and a 3.3V signal in the same bank.

### Step 4: Run the checker before you export anything
`Processing → Start → Start I/O Assignment Analysis`. This is Quartus checking your pin plan for
illegal combinations (wrong voltage in a bank, pin used twice, etc.) before you commit it to a
board. Fix every error here — it is far cheaper than fixing it after fab.

### Step 5: Export the pin file
This is a text file listing, for every used pin: **signal name → ball location (e.g. `AH19`) →
I/O bank → voltage standard.** This file is now the single source of truth for both Quartus and
your schematic — the two must never disagree.

### Step 6: Turn the pin file into a schematic symbol — by script, never by hand
A 2000-ball part cannot be hand-typed into OrCAD without introducing typos that cost you a board
respin. Write a script that reads the pin file and generates an OrCAD-importable symbol,
**split into several smaller "parts"** (this is normal for huge BGAs — even Altera's own
schematic symbols do this):

```
Part A  — Power + Ground (all VCC*, all GND)
Part B  — SDM / Configuration (MSEL, nCONFIG, nSTATUS, CONF_DONE, JTAG, SVID)
Part C  — HPS (only if your device has it)
Part D1 — GPIO Bank 2C
Part D2 — GPIO Bank 2D
Part D3 — GPIO Bank 3A
...one part per GPIO bank you use...
Part E1 — Transceiver Bank 12C (or whichever bank hosts your Ethernet port)
Part E2 — Transceiver Bank 13A (if you use a second bank, e.g. for PCIe)
```

### Step 7: One schematic sheet per part
Exactly like the dev kit schematic you uploaded — a "Power Tree" sheet, a "Clock" sheet, a
"Bank 12C, F-Tile" sheet, etc. Never put everything on one giant sheet.

### Step 8: Wire each pin according to its category
This is the part most people get wrong by treating every pin the same way. It isn't — see Part 2.

---

## Part 2 — Wiring Rules by Pin Category, With Circuit Snippets

### A. Power pins (VCC, VCCP, VCCIO_x, VCCH_x, VCCA_x, VCCL_x, VCCERT_x...)

**What they are:** each one feeds a specific internal function — core logic, PLLs, I/O buffers,
transceiver analog circuits. Different rail, different regulator, different decoupling zone.

**Why it matters:** get the wrong voltage on the wrong rail, or skip decoupling, and the part
either won't configure or will have noisy, unreliable transceivers.

**Snippet — one rail, done correctly:**

```
        3V3_STBY  (your always-on housekeeping rail)
             |
       +-----+-----+
       |  ISL80101  |   <- point-of-load regulator for THIS rail only
       |  or similar |
       +-----+-----+
             | Vout (example: 0.9V for VCCH_SDM)
             |
     +-------+---+---+---+---+
     |       |   |   |   |   |
    Cbulk   C1  C2  C3  C4  ...       <- one bulk cap (10-22uF) near the regulator,
     |       |   |   |   |   |           then many small 0.1uF caps spread around
    GND     GND GND GND GND  GND         the actual VCCH_SDM balls on the package
     (each small cap gets its own 2 ground vias, placed as close to the
      pin as the ball pitch allows — this is the "decoupling zone" rule)
             |
      FPGA VCCH_SDM balls (there may be 10-30 balls on this one rail —
      every one of them needs a nearby cap, not just one cap for the whole rail)
```

**Simple rule:** count how many balls share a rail name in your pin file. That's roughly how many
decoupling caps you need, split between "close to the package" and "near the regulator."

---

### B. Ground pins (GND)

**What it is:** every GND ball ties to one continuous ground plane in your PCB stack-up. No
component needed.

**Why it still needs care:** on a fine-pitch BGA, if you don't put a via right at (or very near)
every GND ball, you create a weak return path for the high-speed signals routed nearby. Put a
via under or immediately adjacent to every GND ball — this is called "via-in-pad" or a stitching
via depending on your fab's capability.

```
   FPGA GND ball
        |
       via  <- straight down into the ground plane layer, as short as possible
        |
   [continuous ground plane]
```

---

### C. Configuration / SDM pins

These control how the chip boots. Get any of these wrong and the device simply never configures
— it isn't a "fix it in firmware" problem.

**MSEL[2:0] — mode select straps.** Tie each directly to logic 1 (VCCIO_SDM) or logic 0 (GND) to
pick your boot mode, or put them on a switch if you want to change modes without reflow:

```
Example: wired for AVSTx16 mode (MSEL2=1, MSEL1=0, MSEL0=1)

   VCCIO_SDM (1.8V)              GND
        |                          |
        +---- MSEL2                |
                                    +---- MSEL1
   VCCIO_SDM (1.8V)
        |
        +---- MSEL0

(If you want it switchable for lab bring-up, replace each direct tie with a
 3-pin header or DIP switch position, exactly like the dev kit's MSEL DIP bank.)
```

**nCONFIG, nSTATUS, CONF_DONE** — these are the "handshake" pins during boot. Each typically
needs a pull-up resistor (check the exact value in the Configuration User Guide for your device)
so the line sits at a known level when nothing is actively driving it:

```
   VCCIO_SDM
        |
      [10K]
        |
        +------ nSTATUS  (also route to your sequencer/MCU so it can read boot status)
```

**JTAG (TCK, TMS, TDI, TDO, TRST)** — standard chain wiring, plus a header so you can actually
plug a programmer in:

```
  Programmer -- TCK ----[series R, e.g. 22-33 ohm]---- FPGA TCK
  Programmer -- TMS ----[series R]---------------------- FPGA TMS
  Programmer -- TDI ----[series R]---------------------- FPGA TDI
  FPGA TDO   ------------------------------------------> Programmer TDI (next device, or back to programmer)
  (TRST, if present, usually gets a pull-up so it defaults inactive)
```

**SVID (SmartVID) pins — only if your OPN is a V/E/X part.** These are just I2C — SDA and SCL —
but they go to one specific place: the SmartVID-capable regulator's SVID interface (see the
earlier walkthrough, §1.3), not to your general housekeeping I2C bus.

```
   FPGA SVID_SCL ------------------- SmartVID regulator SCL1
   FPGA SVID_SDA ------------------- SmartVID regulator SDA1
   (Both lines need pull-ups to the SVID bus voltage — usually 1.8V — check
    the regulator's datasheet for the exact value it expects.)
```

---

### D. Reference clock differential pairs (feeding the transceiver PLLs)

**What it is:** a very clean oscillator or clock synthesizer output, routed as a matched
differential pair straight to the FPGA's dedicated reference clock input balls.

**Why it's different from a normal signal:** any noise or jitter here directly becomes jitter on
every transceiver lane that uses this clock. This is not a "just connect it" pin.

```
   +--------------+
   |  Low-jitter   |---- P ----+========+  (differential pair, controlled impedance,
   |  oscillator   |---- N ----+========+   matched length between P and N — keep the
   |  (e.g. HCSL)  |                         mismatch under about 5 mils / 0.13mm)
   +------+--------+
          |
        0.1uF bypass right at the oscillator's power pin
          |
         GND
                                    |        |
                                    v        v
                            FPGA REFCLKx_P  FPGA REFCLKx_N
                       (goes to ONE quad of transceiver channels —
                        check the F-Tile/E-Tile guide for which physical
                        lanes that quad's reference clock actually serves)
```

**Simple rule from the dev kit schematic:** one reference clock pair per quad (4 channels) you
actually use. If your Ethernet port uses 2 quads, you need 2 reference clock inputs landing at
the right pins — not one clock fanned out in software.

---

### E. High-speed transceiver TX/RX pairs (to your QSFP/QSFP-DD cage)

```
   FPGA TX+ ----+========================+---- Cage TX+
   FPGA TX- ----+========================+---- Cage TX-
                  (controlled-impedance differential pair,
                   length-matched, minimal vias, backdrilled
                   if this trace crosses more than ~4-6 layers
                   at multi-Gbps rates)

   Cage RX+ ----+========================+---- FPGA RX+
   Cage RX- ----+========================+---- FPGA RX-
```

**Key point learned directly from the dev kit:** for a standard QSFP/QSFP-DD module, you do
**not** add AC-coupling capacitors on your board — the module already has them internally, and
the FPGA's on-die termination handles the 100-ohm differential termination. Adding your own caps
here would be redundant and could actually hurt signal integrity. (If you're doing chip-to-chip
SerDes with no module in between, that's different — check that specific interface's
requirements, since some do need external AC coupling.)

---

### F. General-purpose I/O (LEDs, resets, status signals, low-speed control)

**What matters:** match the I/O standard to whatever voltage that bank is set to (from Step 3),
and add the simple support components the function needs:

```
  LED example:
    FPGA GPIO ----[330 ohm]---- LED ---- GND
    (current-limiting resistor sized for the LED's forward current, e.g. ~10mA)

  Reset button example:
    VCCIO
      |
    [10K pull-up]
      |
      +------ FPGA RESET pin
      |
    [Button] ---- GND
    (button pulls the line low when pressed; pull-up holds it high otherwise)
```

---

### G. Unused pins — the pins people forget

Every FPGA package has pins that must be tied a specific way even if you don't use the function
— some must go to ground, some must be left floating, some must tie to a specific rail. **This is
listed pin-by-pin in the Pin Connection Guidelines document — you cannot guess this.** Go through
every unused pin in your pin file against that document before schematic review. This is exactly
the kind of thing Altera's own Schematic Review Worksheet (mentioned earlier) is designed to
catch — use it as your checklist.

---

## Part 3 — Quick Checklist Before You Call the Schematic Done

1. Every signal in your Quartus pin file has a matching net name in the schematic — no
   mismatches, no typos (this is why you scripted the symbol instead of hand-typing it).
2. Every VCCIO bank has exactly one voltage, and every pin in that bank uses an I/O standard
   compatible with it.
3. Every power rail has decoupling caps placed near the actual balls it feeds, not just near the
   regulator.
4. MSEL, nCONFIG, nSTATUS, CONF_DONE, and JTAG are all wired — none floating.
5. If your OPN is SmartVID (V/E/X), SVID_SDA/SCL go to your SmartVID regulator, with correct
   pull-ups, plus the remote-sense pair routed back from the FPGA's own sense balls.
6. Every reference clock pair lands at the correct quad for the transceiver channels it's meant
   to serve.
7. Every unused pin is tied according to the Pin Connection Guidelines — not left as a guess.
8. Run the whole thing against Altera's Schematic Review Worksheet before layout starts.

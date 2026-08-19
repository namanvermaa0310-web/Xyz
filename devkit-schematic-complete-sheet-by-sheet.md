# Agilex 7 F-Series (2× F-Tile) Dev Kit Schematic — Complete Sheet-by-Sheet Walkthrough

**Document:** 180-0330678-D1, Rev D1 · **Board:** DK-DEV-AGF027F1ES (ES) / DK-DEV-AGF027FA (Prod)
**Device:** AGFB027R24C2E2VR0 — F-Series, 027 density (2,692,760 LE), R24C package (2340-ball BGA,
45mm × 42mm), tile speed 2, core speed 2, Extended temp, **V = SmartVID**, dual F-Tile.

The schematic has 58 sheets. This walks all of them, grouped by subsystem, with what each does,
why it exists, and whether you need it.

---

## GROUP A — Front Matter (Sheets 1–2)

### Sheet 1 — Cover, Table of Contents, Notes
Board photo with every major connector called out: two DDR4 x72 DIMM/DDR-T slots, QSFP-56 cage
(200GbE marked), QSFP-DD cage (400GbE marked), PCIe over SAS x4 (MCIO), CXL connector, HPS IO48
mezzanine site, JTAG header, USB Blaster, MAX10 system controller, MSEL switch bank, PCIe REFCLK
control switch, 1PPS SMA connectors, user LEDs, reset buttons, 150W power switch, fan header.

Mechanical constraints noted: **max height dual-slot — top 55.12mm, bottom 2.67mm**, 5mm clearance
under the HPS daughter card.

Also the board/device OPN cross-reference table:

| Name | Official Name | Board OPN | Device OPN |
|---|---|---|---|
| FM86 FPGA DK (ES) | Two F-Tile Edition (ES) | DK-DEV-AGF027F1ES | AGFB027R24C2E2VR0 |
| FM86 FPGA DK (Prod) | Two F-Tile Edition (Prod) | DK-DEV-AGF027FA | AGFB027R24C2E2V |
| FM76 FPGA DK | Two F-Tile + High-Performance Crypto | DK-DEV-AGF023FA | AGFD023R24C1VC |

**Note the third row** — that's the production, hard-crypto variant (density 023, core speed 1)
we discussed as the better dev-kit choice.

### Sheet 2 — Block Diagram
Single-page system view: the 2340-pin BGA in the center, two F-Tiles on the left edge feeding
MCIO×4, QSFP-56 (4 TX/RX), and QSFP-DD-56 (8 TX/RX); banks 2C/2D/2F to the DDR4 DIMMs; banks
3A/3B/3C/3D to LEDs, switches, IO expander, HPS DDR4 component; SDM to MAX10 and QSPI flash;
MAX10 connected to board power circuitry, clock circuitry, MAC EEPROM, temp sensors; PCIe x16
gold fingers at the bottom.

**Note the "FM86 only" shading** on banks 3B, 3C and part of 2E — those banks don't exist on the
smaller FM76 package. This is the package-migration planning discussed earlier.

---

## GROUP B — Power (Sheets 3, 7, 43–58) — the largest group, and the most reusable

### Sheet 3 — Power Tree Diagram
The master one-page view of every rail. Two 12V sources enter at the left (12V_PCIE +/-8% 60W
from the edge connector, and 12V_AUX +5/-8% 150W from the ATX connector), each through a
**MAX16545** hot-swap controller. From there:

- **12V_PCIE_IN** → 12V_DDRT, and via LTM4662 regulators → **5V0** and **3V3**
- **12V_AUX_IN** → a **4-phase LTM4664 + LTC7051 (40A/phase)** stage generating the big core rail:
  **VCC / VCCP / VCCL_HPS** at V/O 0.8V ±3%
- A separate **1-phase** stage → VCC_HSSI_GXF, VCCL_SDM, VCCH at 0.8V ±3%
- **MAX10/LTC10 Controller Sequencer** driving EN_G0/G1/G2/G3 and reading PG_G0..G3
- Cascade of smaller rails: 3V3_MAX10, 2.5V_MAX10, 1.8V_MAX10, 1.2V_MAX10 (housekeeping),
  VCCH_SDM (0.9V ±2.5%), VCCERT_FGT_GXF (1.0V ±2.5%), VCCPT (1.8V ±3%), VCCR_CORE (1.2V ±5%),
  VCCA_PLL, VCCIO_1V2, VCCIO_PIO_SDM/DDR4, VCCIO_SDM/HPS/VCCBAT/VCCFUSEWR_SDM (1.8V ±2.7%),
  and DDR4-specific 2.5V_DDR4_DIMM/COMP plus 0.6V VTT/VREF for both DIMM and component memory.
- QSFPDD_PWR_EN and QSFP_PWR_EN gate 3V3_QSFPDD and 3V3_QSFP through TPS2557 load switches.

**This is the single most valuable sheet to study** — it shows the complete rail hierarchy and
which rail derives from which, at a glance.

### Sheet 7 — Power Sequence Diagram
The timing/order companion to sheet 3. Documents which enable group each rail belongs to and the
PG dependency chain.

### Sheet 43 — 12V_PCIE and 12V_AUX Hot Swap Controllers + Power On Switch
Two near-identical **MAX16545BGPF / MAX16550AGPN** circuits (QFN-22), I2C addresses **0x42**
(PCIe) and **0x40** (AUX).

Design values printed directly on the schematic — genuinely useful as worked examples:
- 12V_PCIE = 75W = 12V @ 6.25A; 12V_AUX = 150W = 12V @ 12.5A
- Ios limit = 3.656A / 4.039A / 4.349A; internal FET Rds(on) max 35mΩ; dropout at 4A = 0.14V
- Internal FET power loss max = 668mW (Rds(on) 1.9mΩ for MAX16550, 0.95mΩ for MAX16545B)
- Overcurrent tiers: MIN 16.18A/194W, TYP 18.39A/220.68W, MAX 20.6A/247.2W
- UVLO threshold scaling table: at 12V input, EN = 1.47V; at 10V, 1.23V; at 16V, 1.96V
- Soft-start Tr = 6ms; ILoad reporting = 5µA per amp

**Power On Switch logic**, stated plainly on the sheet:
> When 12V PCIe slot power **and** 12V AUX power are both provided (Add-in Card Mode), the ON
> switch has no function. When no 12V PCIe slot power is available (Bench Mode), the ON switch
> is used to power the board on/off.

An **LTC4357** ideal-diode controller with an FDMS86163 FET ORs the two 12V sources together.

### Sheet 44 — 5V0 Generation + MAX10 Housekeeping Rails
**LTM4625** µModule generates 5V0 from 12V_AUX_IN. RUN pin threshold table given for various
input voltages (at 12V input, RUN = 1.85V). Vout = 0.6V × (1 + 60.4kΩ/Rfb). TSS = 3ms soft-start.
1MHz switching, forced-continuous mode, 2-phase.

Then three **ISL80101** DFN-11 regulators generate **MAX_2V5 (80mA), MAX_1V8 (50mA), MAX_1V2
(200mA)** — the housekeeping rails that power the MAX10 sequencer itself. Each uses
Vout = 0.5V × (Rup/Rdn + 1), VDO max 185mV, EN threshold 0.3/0.8/1V.

Their PG outputs (PG_MAX_2V5, PG_MAX_1V8, PG_MAX_1V2) plus 3V3_STBY_PG feed into **MAX10_NCONFIG**
— i.e. MAX10 doesn't come out of reset until its own power is confirmed good. That's the
bootstrap of the whole sequencing chain.

### Sheet 45 — 3V3 Generation
**LTM4620A** dual µModule (12V_PCIE_IN → 3.3V ±5%). Feeds 3V3_STBY. A **PSMN1R0-30YLC** FET
(20A, Rds(on) 1.5mΩ, 0.6W max) with an **LTC1981** gate driver switches 3V3_STBY → 3V3_SYS,
gated by EN_G0 — so the standby rail comes up first and the system rail is sequenced after.
Remote sense routed as a differential pair back to the load.

### Sheet 46 — LTC3888: the SmartVID core regulator (**most important sheet for your design**)
**LTC3888**, I2C address **0x55**, controlling the FPGA VCC rail.

- **LTC Programming Header** (J13, DF3-4P-2DSA: SDA/GND/SCL/3.4V) for factory NVM programming
- **SHARE VID I2C with MAX10 TELEMETRY I2C** — the schematic explicitly notes
  *"Agilex supports Multi-master"*, so the SVID bus and MAX10's telemetry bus are the same
  physical bus with two masters.
- FPGA_3V3_SVID_SDA / FPGA_3V3_SVID_SCL connect to the FPGA's SDM pins (via the level shifters
  on sheet 38)
- **Remote sense**: FPGA_VCCSENSE / FPGA_GNDSENSE routed back as a differential pair from the
  FPGA package — schematic note: *"Route as diff pair. Remote sense at the FPGA VCC gate routing"*
- A second remote-sense pair for FPGA_VCC_HSSI_SENSE / FPGA_VCC_HSSI_GNDSENSE
- Drives PWM0–PWM4 and ISENSE0–ISENSE4 out to five separate power stages (sheets 47–51)
- Configured for 4+2 phases, 500kHz; Vout = 0.80V at startup; RSET uses a required 18.7kΩ
- Outputs FPGA_VCC_PG and FPGA_VCC_HSSI_GXF_PG back to the sequencer

### Sheets 47–51 — LTC7051 Power Stages (the actual current delivery)
Five near-identical sheets, each an **LTC7051** dual power stage with a **72nH FP0805R1-R07-R**
inductor:

| Sheet | Rail | Output caps |
|---|---|---|
| 47 | FPGA_VCC (phase 1) 0.8V ±3% | 7× 100µF 6.3V 1206 + 47µF |
| 48 | FPGA_VCC (phase 2) 0.8V ±3% | same |
| 49 | FPGA_VCC (phase 3) 0.8V ±3% | same |
| 50 | FPGA_VCC (phase 4) 0.8V ±3% | same |
| 51 | FPGA_VCC_HSSI_GXF 0.8V ±3% | same, plus remote sense to FPGA |

Each has a thermal-sense transistor (MMBT3906) placed physically near the inductor — schematic
says *"Place near L10"* etc. — for per-phase temperature telemetry back to the LTC3888.

### Sheet 52 — VCCH_SDM (0.9V ±3% @ 1.3mA)
Tiny ISL80101 rail. Note the current: **1.3mA**. Not every FPGA rail is high-current — size the
regulator to the actual load, don't default to a big part.

### Sheets 53–54 — LTC7132 Dual Digital Regulators
Two LTC7132 parts (I2C **0x74** and **0x75**), each generating two rails:

- **Sheet 53 (0x74):** FPGA_VCCERT_FGT_GXF 1.0V ±2.5% (L10, 0.33µH XAL5030) and FPGA_VCCR_CORE
  1.2V ±5% (L11, 0.33µH). Config by resistor, overridable by NVM. FREQ = 500kHz, PHASE = 0/180.
- **Sheet 54 (0x75):** FPGA_VCCPT 1.8V ±3% (L12, 0.47µH XEL4030) and FPGA_VCCIO_1V2 1.2V ±5%
  (L13, 0.33µH), the latter including VBAT and VCCFUSEWR_SDM.

Both have "Place near L1x" thermal transistors and an LTC programming header (J107). ADI_ALERTn
is shared — schematic warns *"Do not place on END of daisy chain."*

### Sheet 55 — VCCFUSEWR_GXF (1.0V ±3% @ 200mA)
Single ISL80101. Small rail, but it exists and must be present.

### Sheet 56 — VCCBAT Battery Circuit
**Explicitly labelled: "VCCBAT Circuit intended for FM76 Device with Crypto Support."**

CR1620 coin cell + BAT54 diodes ORing with 3V3_STBY, an LT1389 1.25V reference, and a resistor
selection (share pad, only one resistor installed at a time) choosing between ~1.25V and 1.8V.
Current draw ~0.8µA from the battery, ~6.4µA from the rail.

**This is the battery-backed key storage rail for hard-crypto devices.** If your OPN has hard
crypto and you intend to use volatile key storage, you need this circuit. If your fabric code has
no crypto blocks, you can omit it.

### Sheet 57 — VCCIO_SDM_HPS 1.8V ±2.7% @ 10mA
ISL80101 again. Collects PG from VCCR_CORE, VCCIO_1V2, and the DDR4 LDOs into PWRGD_G3, the last
sequencing group.

### Sheet 58 — DDR4 VTT/VREF Generation
Four regulators: two ISL80101 (2V5_DDR4_DIMM, 2V5_DDR4_COMP) and two **TPS51200** VTT
termination regulators (0V6_DDR4_VTT_DIMM, 0V6_DDR4_VTT_COMP) with their VREF outputs.
Alternate part noted: ON-Semi NCP51200. All four PGs collect into PG_DDR4_LDO.

---

## GROUP C — Clocks (Sheets 4, 35, 36)

### Sheet 4 — Clock Diagram
One-page overview. **ZL30733** IEEE 1588 clock (U23) is the hub, feeding: PPS_FPGA_CLKOUT,
ToD_MASTER_CLK_125M, CLK_FPGA_100M, PTP_SAMPLE_CLK_250M → Bank 3A; DDR4_DIMM1/DIMM2/COMP_REFCLK
→ Banks 2C/2F/3D; QSFPDD_REFCLK, QSFP_REFCLK, CIPRI_HIGH/LOW → Bank 12C. Separately, **Si52204**
(U25) + **Si53254A** (U26, U27) generate PCIe/CXL reference clocks → Banks 12C and 13A. A 125MHz
oscillator feeds FPGA_OSC_CLK1 → SDM.

### Sheet 35 — Clock 1 (ZL30733 detail)
I2C address **0x70**. VDDIO can be 1.8V or 3.3V — schematic explains the tradeoff:
> When 3.3V, OUT0 clock will be FPGA clock. When 1.8V, OUT0 can be 1.8V LVCMOS to drive 100MHz SE
> on the P leg and FPGA 1PPS clock on the N leg — for this case, the optional dividers
> (R6840–R6843) should match the FPGA clock input requirements.

Output types per pin annotated (LVDS, PROG_DIFF). Optional TCXO or OCXO footprint provided —
**overlapping footprints, install only one** — for higher-stability timing applications. An
**ISL80101** generates ZL30733_1V8 (1.8V @ 367mA). GPIO3_AC3/GPIO4_AC4 must be low at the rising
edge of RST_B.

### Sheet 36 — Clock 2 (PCIe/CXL clocking)
**Si52204** (U25, I2C **0x6A**) with a 25MHz crystal (Y1), spread-spectrum select via SW4.1:
> SW4.1 Open = No Spread Spectrum; SW4.1 Close = 0.5% Down Spread

FS pin selects frequency (100MHz default / 200MHz / 133MHz) and SS (0.25% / OFF default / 0.50%).
Two **Si53254A** 2:1 clock muxes (U26 for PCIe, U27 for CXL) select between the local Si52204
output and a connector-supplied clock, controlled by SW4.2–SW4.4 (OPEN = HIGH, CLOSE = LOW).
IMP_SEL selects 100Ω (default) or 85Ω output impedance. Termination is on-chip; external
termination provided only for tuning.

---

## GROUP D — MAX10 System Controller & Configuration (Sheets 5, 6, 10, 11, 40, 41)

### Sheet 5 — I2C Diagram
The complete I2C topology. MAX10 (U5) is master over two buses (FX2_SCL/SDA and MAX_I2C_SCL/SDA),
with the address map:

| Device | Address |
|---|---|
| LTC3888 (VCC, VCCHSSI) | 0x55 |
| ZL30733 clock | 0x70 |
| Si52204 clock | 0x6A |
| MAC EEPROM (M24128) | 0x57 / 0x5F |
| Cert EEPROM (M24128) | 0x56 / 0x5E |
| MAX16545B PCIe HSC | 0x42 |
| MAX16545B AUX HSC | 0x40 |
| LTC7132 (VCCPT etc.) | 0x74 |
| LTC7132 (VCCERT etc.) | 0x75 |
| MAX31730 temp sensor 1 | 0x38 |
| MAX31730 temp sensor 2 | 0x9A |
| IO expander (PCA9534) | 0x4E |
| FRUID EEPROM | 0xA0 |

The FPGA has its own I2C branch (FPGA_SCL/SDA) plus a dedicated **FPGA SVID to VCC Regulator**
line — the SmartVID path.

### Sheet 6 — JTAG Diagram
USB-Blaster II dongle → 10-pin header (J3) → MUX (U6) → MAX10 → FPGA JTAG chain, with
USB_MAX_JTAG_SEL and MAX10_JTAG_EN controlling the path. Also shows PCIe/CXL PERSTn routing with
level shifting (1.8V→1.2V and 3.3V→1.2V) into the FPGA.

### Sheet 10 — MAX10 System Controller (the big one)
Every MAX10 connection: AVSTx16_D[15:0]+CLK+VALID+READY to the FPGA; FPGA_NCONFIG, NSTATUS,
INIT_DONE, CONF_DONE, CVP_CONF_DONE, MSEL[2:1]; ASx4 interface to 2Gb QSPI flash; power circuit
EN_G[3:0], PWRGD_G[3:0], LTC_FAULT_N, LT_SPI_Bus, SVID; two QSPI flash interfaces (FLASH0, FLASH1);
NIOS flash; temp sensor THERMn inputs; QSFP/QSFPDD PWR_EN, PWR_FAULT_N, MODPRS_N; PCIe PWRBRKN,
PERSTN, JTAG; HPS daughter card signals; USB PHY interface.

### Sheet 11 — FPGA Configuration (SDM detail) — **critical reference**
The MSEL strap table, directly from the sheet:

| Config Mode | MSEL2 | MSEL1 | MSEL0 |
|---|---|---|---|
| JTAG | 1 | 1 | 1 |
| AVST x16 | 1 | 0 | 1 |
| AS x4 Fast (CVP) | 0 | 0 | 1 |
| AS x4 Norm | 0 | 1 | 1 |

Set by SW3, where **Close = 0, Open = 1**, and MSELx reads 0.164V when SW3 is closed.

**SDM pin voltage thresholds** (the numbers you need for level-shifting decisions):
```
Vil = 0.35 × VCCIO_SDM = 0.63V
Vih = 0.65 × VCCIO_SDM = 1.17V
Vol = 0.4V
Voh = VCCIO_SDM − 0.4V = 1.4V
```

**CATTRIP** = 0 if FPGA die temp ≥ 120°C — MAX10 must shut down the FPGA on this signal.

Full SDM_IO function assignment listed (SDM_IO0 = PWRMGT_SCL, SDM_IO11 = PWRMGT_SDA,
SDM_IO16 = PWRMGT_ALERT, SDM_IO5 = AS_nCSO0, SDM_IO7 = AS_nCSO2, etc.), plus RREF_SDM, VREFP/VREFN,
VSIGP/VSIGN pairs, and TEMPDIODEA0n/p — the latter with the note
*"Route as a diff pair and avoid other nearby signals."*

### Sheet 9 — FPGA Configuration Flash
Two **MT25QU02GCBB8E12** 2Gb dual-die QSPI flash devices at 1.7–2.0V (absmax 4V).
> AGILEX AGF027 Bit Stream size = 833.4Mbits, total FPGA image support = 4
> These are dual-die flash so each device is 2 loads on MAX10 IOs

### Sheet 40 — MAC EEPROM, Cert EEPROM, NIOS Flash
Two M24128 128Kb I2C EEPROMs (MAC at 0x57/0x5F, Cert at 0x56/0x5E) and a **W25Q64JVSSIM** 64Mb
SPI flash for the Nios V soft processor image.

### Sheet 41 — USB-Blaster II Phy
**CY7C68013A** USB controller (48MHz IFCLK, 24MHz crystal), **TPD2EUSB30** ESD protection on
D+/D−, **MAX811** reset supervisor, micro-USB connector. This is a complete on-board JTAG
programmer.

---

## GROUP E — FPGA Banks (Sheets 12–24, 42)

### Sheets 12–15 — DDR4 Banks 2C, 2D, 2E, 2F
Each sheet maps one bank's DQ/DQS/DBI/CDR pins to DDR4 DIMM signals, organized by byte lane
(Byte0: 0–7, Byte1: 8–15, etc.). Includes RZQ calibration resistor placement (*"Place near RZQ
pin"*) and external termination options (*"External termination for tuning since OCT ±40Ω
accuracy; OCT should be turned off when external termination is used"*).

Migration notes: **Bank 2C and 2D available in both FM86 and FM76**; **Bank 2E available only in
FM86** (hence *"DIMM2 not available in FM76 Devkit version since Bank2E does not migrate"*);
Bank 2F available in both.

### Sheet 16 — Bank 3A (General I/O)
AVSTx16 configuration data bus, FPGA LEDs, QSFP/QSFPDD control via **PCA9534** IO expander
(0x4E, note: *"P0–P7 default to input after power-on; user must configure these IOs"*), 1PPS
SMA test points, PTP clocks, MAX10 spare bus. Half of this bank doesn't migrate to FM76.

### Sheets 17–19 — Banks 3B, 3C, 3D
3B: FM86 only, unused I/O. 3C: only half migrates to FM76. 3D: DDR4 component interface
(available in both packages).

### Sheet 20 — **Bank 12C, F-Tile** (the most relevant sheet for your project)
Maps one F-Tile bank to three interfaces simultaneously:
- **CXL PCIe×4** → FGTL12C_RX/TX_Q0_CH0–CH3
- **QSFP-56** (4 lanes) → FGTL12C_RX/TX_Q1_CH0–CH3
- **QSFP-DD-56** (8 lanes) → FGTL12C_RX/TX_Q2 and Q3 (all 8 channels)

Plus the reference clock inputs, per quad: REFCLK_FGTL12C_Q0/Q1/Q2/Q3_RX_CH0–CH9.

The **FGT and System PLL Reference Clock Network** diagram on this sheet is exceptionally useful
— it shows four FGT quads (Quad0=CXL/PCIe×4, Quad1=QSFP, Quad2 and Quad3=QSFPDD) fed by ten
reference clocks:

| Clock | Purpose | Frequency |
|---|---|---|
| Reference Clock 0 | CXL local board REFCLK | 100MHz |
| Reference Clock 1 | CXL REFCLK from connector | 100MHz |
| Reference Clock 2 | QSFP global REFCLK | 156.25MHz |
| Reference Clock 3 | Global REFCLK for CPRI low rates | 153.6MHz |
| Reference Clock 5 | Global REFCLK for CPRI high rates | 184.32MHz |
| Reference Clock 6 | QSFPDD REFCLK | 156.25MHz |
| Reference Clocks 4, 7, 8, 9 | additional regional/local | — |

Color-coded as **Global** / **Regional** / **Local (bidirectional)** reference clocks, feeding
three System PLLs. Several P/N pairs are swapped for routing convenience with the note
*"P/N swap for routing, IP must reswap"* — a legitimate layout technique, but you must
compensate in the IP configuration.

### Sheet 21 — Bank 13A, F-Tile (PCIe)
PCIe Endpoint ×16 to the gold-finger connector, using FGTR13A_RX/TX_Q0–Q3_CH0–CH3, with its own
independent reference clocks (REFCLK_FGTR13A_Q0–Q3).

### Sheet 22 — HPS
All 48 HPS_GPIO signals with their multiplexed functions (SPI, UART, I2C, NAND, USB, EMAC,
SDMMC), plus temp diode pairs and the ENB_GXF_FHT12C/13A pins — note **FHT Not Used** on this
board, confirming it uses FGT lanes only.

### Sheets 23–24 — FPGA Power and GND pins
Complete enumeration of every VCC/VCCP/VCCIO/VCCH/VCCA/VCCL/VCCERT ball (sheet 23) and every GND
ball (sheet 24). Hundreds of pins each. This is what your scripted symbol generator produces.

### Sheet 42 — FPGA Decoupling
Two sections with explicit placement zones:
- **FPGA Core Decoupling**: "Place on BOT Side" / "Place on TOP Side", "Place these caps within
  FPGA cavity for VCC, VCCP", "Place these caps around FPGA pin field", "Extra FPGA VCC Caps —
  Place on BOT side periphery, 2-GND vias each cap gnd pad minimum", "Extra FPGA VCC Caps —
  Place on BOT side in BGA field between PWR/GND vias"
- **FPGA F-Tile Decoupling**: per-bank placement — "Place BOT near VCC_HSSI_GXF pins",
  "Place on left side near F-Tile Bank 12C", "Place on left side near F-Tile Bank 13A"

---

## GROUP F — Connectors and Interfaces (Sheets 8, 26–34, 39)

### Sheet 8 — PCIe Endpoint Edge Connector
Full ×16 gold finger pinout with FRUID EEPROM (0xA0, 400kHz max), presence-detect straps
(PRSNT2n for x1/x4/x8/x16), 0.22µF AC coupling caps on RX lanes. Note:
> When PWRBRKn is asserted, MAX10 can reduce power by clearing FPGA programming and stopping all
> clocks. When deasserted, MAX10 can reconfigure FPGA and re-enable all clocks.

Lane reversal is permitted during layout for both TX and RX.

### Sheets 25–30 — DDR4 DIMM and Component Interfaces
Sheet 25: DDR4/DDR-T DIMM pin map (with the note that DDR-T repurposes five pins: CS1#→Grant,
CKE1→Request, ODT1→Error, CLK1/CLK1#→Early Read ID). Sheets 26–27: DIMM1 and DIMM2 connectors
(I2C 0xA0/0xA1). Sheets 28–30: five soldered **MT40A2G8VA** DDR4 components with termination.

### Sheet 31 — HPS IO-48 Daughter Card Connector
QSH-030-01-F-D-A 60-pin connector carrying all HPS GPIO plus power (12V, VCCPT) and control.

### Sheet 32 — **QSFP-DD-56** (directly reusable for your design)
**Amphenol UE36-A1015-2000T** connector + **UE36-C16221-06A4A** cage. Key notes:
> NOTE 1: Bypass capacitors should be placed as close to the associated 20-pin connector as possible.
> NOTE 2: zQSFP 100-ohm termination is implemented via the FPGA on-chip termination.
> NOTE 3: DC blocking capacitors are in the module for RX and TX.
> NOTE 4: 1µH inductors should have a DC resistance of less than 0.1-ohm.

**TPS2557** load switch with ILIM formulas printed on-sheet:
`I_OSmax(mA) = 99038V/R_ILIM^0.947kΩ`, nominal `111704V/R_ILIM^1.062`, min `127981V/R_ILIM^1.179`.
Ios limit = 3.656A/4.039A/4.349A; Rds(on) max 35mΩ; dropout at 4A = 0.14V.
Control signals: MODSELL, RESETL, MODPRSL, INITMODE, INTL, SCL, SDA, plus VS1/VS2/VS3 straps.
Three 1µH inductors filter QSFPDD_VCC, VCCR, and VCCT separately.

### Sheet 33 — QSFP-56
Same architecture, 4 lanes: **Amphenol FS1-K38-20ZA-A0** connector + **U95-T1C1-101A** cage,
second TPS2557 with identical ILIM math.

### Sheet 34 — CXL Connector
G97R12322HR connector with **MAX3378E** level shifter. Notes explain it's designed for the FM85
devkit M.2 daughter card (M-Key, PCIe×4 + SATA), that FM86 CXL channels connect to M.2 channels
8–11, and that **I2C is not supported** on this interface.

### Sheet 39 — LEDs and Pushbuttons
Power LED (blue), overtemp LED (red), four user green LEDs, QSFP/QSFPDD status LEDs (yellow +
green each), CONF_DONE LED, config image select LEDs. Five pushbuttons: CXL_PERSTn, CPU_RESETn,
HPS_RESETn, PCIE_PERSTn, GXF_2ND_PERSTn — each with 1000pF debounce caps, with a warning:
*"DNI cap loading for U33 to work properly"* on one of them. Also documents the large mounting
hole positions (front, rear, heatsink, IO48 daughter card).

---

## GROUP G — Support Circuits (Sheets 37, 38)

### Sheet 37 — Buffers, Translators, Board Temp Sensors
- Two **SN74LVC2G17** Schmitt buffers cleaning up JTAG TCK/TMS/TDI/TDO between PCIe and MAX10
- **MAX31730** temp sensor EU1 (0x38) monitoring FPGA TEMPDIODE0 (Core A), F-Tile 12C, F-Tile 13A
- **MAX31730** EU2 (0x9A) monitoring Core C, inlet temp, outlet temp (with MMBT3906 transistors
  placed at the physical inlet and outlet — *"Place at Inlet" / "Place at Outlet"*)
- Note: *"Route all DXPx/DXNx pairs as a diff pair and avoid other nearby signals"*
- **TXS0108E** 8-bit level shifter for the LTC SPI bus (3.3V ↔ 1.8V)
- **NLSX4014** level shifters for LED signals and reset signals (1.2V ↔ 3.3V)

### Sheet 38 — I2C Translators
Four **PCA9306** dual bidirectional level translators bridging voltage domains:
- FPGA_1V8_SVID ↔ FPGA_3V3_SVID (the SmartVID path to LTC3888)
- FPGA_1V2 ↔ DDR4_2V5_DIMM
- DDR4_2V5_DIMM ↔ FPGA_3V3
Each with the note *"SCL/SDA are Hi-Z when EN is low"* and channel Icc = 128mA max.

---

## What to take from each group for your custom board

| Group | Sheets | Copy? |
|---|---|---|
| Power tree structure | 3, 7 | **Yes — study first, adapt rail list** |
| Hot swap | 43 | **Yes**, one input instead of two |
| SmartVID (LTC3888 + remote sense) | 46 | **Yes — copy closely if V/E/X OPN** |
| Power stages (LTC7051) | 47–51 | **Yes**, fewer phases for lower core current |
| Small POL rails (ISL80101) | 44, 52, 55, 57 | **Yes**, same pattern, your rail list |
| VCCBAT battery circuit | 56 | Only if hard-crypto OPN + volatile keys |
| DDR4 rails | 58 | Only if you use external memory |
| Clock generation | 4, 35, 36 | **Tier 1 only** — drop PCIe/CXL clock tiers |
| MAX10 sequencer + config | 5, 10, 11 | **Yes**, scaled down; copy MSEL/SDM tables verbatim |
| Config flash | 9 | **Yes**, 1–2 images instead of 4 |
| USB-Blaster II | 41 | Optional — JTAG header may suffice |
| DDR4 banks | 12–15, 25–30 | Only if external memory needed |
| **Bank 12C F-Tile + refclk network** | **20** | **Yes — your Ethernet port template** |
| Bank 13A (PCIe) | 21 | Only if host interface required |
| HPS | 22, 31 | Only if OPN has HPS and you need it |
| FPGA power/GND enumeration | 23–24 | **Yes** — your scripted symbol produces this |
| **Decoupling zones** | **42** | **Yes — highest payoff for lowest effort** |
| **QSFP-DD / QSFP port circuits** | **32, 33** | **Yes — copy near-verbatim** |
| PCIe edge connector | 8 | Only if host interface required |
| CXL | 34 | Drop unless required |
| LEDs/buttons | 39 | **Yes**, simplified |
| Temp sensors + level shifters | 37, 38 | **Yes** — thermal monitoring is not optional |

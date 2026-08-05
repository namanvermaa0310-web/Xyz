# USB Project — Line-by-Line Walkthrough
## Phase 4: The FAT32 Parser (`usb_dev_dir_detect.vhd`)

> This module is *why the device only works with FAT32.* Every filesystem
> limitation we discussed earlier lives here, in specific lines you can now
> point to. Patterns from Phases 1–3 assumed known.

## The one-line interview answer

> "`usb_dev_dir_detect` watches disk sector 0 and the volume boot sector as
> they stream past, and parses the FAT32 layout out of them — where the FAT is,
> where the root directory is, where file data begins. The encryption stage
> needs those region boundaries to know which sectors hold file content versus
> filesystem metadata. It's hardwired to FAT32's on-disk structure, which is
> exactly why non-FAT32 drives aren't supported."

---

## Entity (lines 11–40): in = raw sectors, out = filesystem map

Inputs:
```vhdl
LBAaddr   : IN ...(31 downto 0);   -- which sector is currently being read
cmdtype   : IN ...(7 downto 0);    -- read or write
die_rxactive/valid/data            -- the sector's bytes streaming past
```
Outputs — the "filesystem map" it produces:
```vhdl
LBA_dataregion_o  -- sector where file DATA starts
LBA_fatregion_o   -- sector where the FAT (allocation table) starts
LBA_rootregion_o  -- sector where the root directory starts
secperfat_o       -- sectors per FAT
secpercls_o       -- sectors per cluster
rifmemwe_dir / rifmemwa_dir / rifmemip_dir  -- writes discovered file locations into a RAM
```
- Just from the pinout: input a sector's bytes, output the addresses of the
  FAT/root/data regions. That's the whole job — turn raw disk bytes into "here's
  the map of this filesystem."

## Two parallel state machines (declared lines 45–99)

The module has **three** clocked processes running together:
1. **`state_media`** (lines 46–53) — parses MBR + boot sector to find the regions.
2. **`state_dir`** (lines 76–80) — walks the root directory to find files.
3. A tiny **`pkt_len`** counter (lines 111–129) — counts bytes within the
   current sector, so the parsers know *which offset* each byte is at.

We focus on `state_media` (the region parser) — that's where the FAT32
assumptions live.

## The byte-position counter (lines 111–129)
```vhdl
process(usb_clk_us)
begin if(rising_edge(usb_clk_us)) then
    if(core_reset_n = '0') then pkt_len <= x"000";
    else
        if(die_rxactive = '1') then
            if(die_rxvalid = '1') then
                pkt_len <= pkt_len + 1;   -- count each valid byte in this sector
```
- `pkt_len` is a running **byte offset within the current sector**. Every field
  the parser wants is at a known offset, so the logic is always "when
  `pkt_len` = offset X, grab this byte." Simple and central — you can't parse
  fixed-layout data without a position counter.

---

## The region parser (lines 148–276): the heart of Phase 4

### First: is this sector the MBR, or the volume boot sector? (lines 148–152)
```vhdl
mbrdata <= '1' when LBAaddr = x"00000000" else '0';   -- sector 0 = MBR
voldata <= '1' when LBAaddr = LBA_VOLUME0 else '0';    -- the volume's first sector
```
- Sector 0 of a disk is the **MBR** (Master Boot Record — the partition table).
  The actual filesystem starts later, at a sector the MBR points to
  (`LBA_VOLUME0`). This module has to read *both*: the MBR to learn where the
  volume is, then the volume boot sector to learn the FAT layout.

### `media_idle` → decide what we're looking at (lines 166–175)
```vhdl
when media_idle =>
    if(die_rxvalid = '1') then
        if(mbrdata = '1')    then state_media <= media_analypkt;  -- it's sector 0
        elsif(voldata = '1') then state_media <= media_volume1;   -- it's the boot sector
        else state_media <= media_skip;                            -- something else, ignore
```

### `media_analypkt` → MBR or direct boot sector? (lines 182–190)
```vhdl
when media_analypkt =>
    if(die_rxvalid = '1') then
        if(die_rxdata = x"EB") then         -- first byte 0xEB = a boot-sector jump instruction
            LBA_VOLUME0 <= x"00000000";     -- ...so sector 0 IS the boot sector (no MBR)
            state_media <= media_volume1;
        else
            state_media <= media_mbr1;      -- otherwise it's a real MBR, parse partition table
```
- **`0xEB`** is the first byte of an x86 jump instruction that every FAT boot
  sector starts with. If sector 0 begins with it, the disk is "superfloppy"
  formatted (no partition table, filesystem starts at sector 0). Otherwise it's
  a normal partitioned disk with an MBR. Handling both is a nice robustness
  touch worth mentioning.

### `media_mbr1` / `media_mbr2` → read the partition table (lines 192–212)
```vhdl
when media_mbr1 =>
    if(pkt_len = x"1C3") then partition_type <= die_rxdata; state_media <= media_mbr2; end if;
when media_mbr2 =>
    if(pkt_len = x"1C7") then LBA_VOLUME0 <= die_rxdata & LBA_VOLUME0(31 downto 8);
    elsif(pkt_len = x"1C8") ... x"1C9" ... x"1CA" ...   -- 4 bytes, little-endian
        state_media <= media_skip;
```
- **Offset 0x1C3** = partition type byte; **0x1C6–0x1CA** = the partition's
  starting LBA (4 bytes). These are the fixed offsets of **partition-table
  entry #1** in the MBR. The `die_rxdata & LBA_VOLUME0(31 downto 8)` idiom
  shifts each byte in — and doing it low-byte-first assembles a **little-endian**
  32-bit value (x86/FAT store multi-byte numbers little-endian).
- **Interview-critical limitation right here:** it reads **only partition entry
  #1** (offset 0x1C6). A drive with the filesystem in partition 2/3/4, or a
  **GPT** disk (which has a "protective MBR" and the real partition table
  elsewhere), would be misread. This is the "MBR-only, no GPT" limitation, at an
  exact line.

### `media_volume1` → the FAT32 signature check + geometry (lines 214–238)
```vhdl
when media_volume1 =>
    if(pkt_len = x"001") then if(die_rxdata /= x"EB") then state_media <= media_skip; end if;
    elsif(pkt_len = x"002") then if(die_rxdata /= x"58") then state_media <= media_skip; end if;
    elsif(pkt_len = x"003") then if(die_rxdata /= x"90") then state_media <= media_skip; end if;
    elsif(pkt_len = x"00E") then secpercls <= die_rxdata;                         -- (mislabeled, see note)
    elsif(pkt_len = x"00F") then reserved_sec <= die_rxdata & reserved_sec(15 downto 8);
    elsif(pkt_len = x"010") then reserved_sec <= die_rxdata & reserved_sec(15 downto 8);
    elsif(pkt_len = x"011") then num_fats <= die_rxdata; state_media <= media_volume2;
```
- **THIS IS THE FAT32-ONLY GATE.** Bytes 1–3 of the boot sector must be
  `EB 58 90`. If any byte differs, it bails to `media_skip` and parses nothing.
  - `EB 58 90` is the **FAT32** boot-sector jump signature.
  - **NTFS** starts `EB 52 90` → fails at byte 2 (`58` check). Skipped.
  - **exFAT** starts `EB 76 90` → fails at byte 2. Skipped.
  - **FAT16/FAT12** use different jump bytes and a different field layout →
    skipped, and even if not, the FAT32-specific offsets below would be wrong.
  - **The comment on line 270 literally says "As per FAT32 sector."**
  - *This is your entire "FAT-only" answer, in three lines.* In an interview:
    "I validate the FAT32 boot signature `EB 58 90` and reject anything else,
    because the field offsets I parse are FAT32-specific."
- Then it grabs the geometry: **reserved sectors** (offset 0x0E–0x0F — the code's
  offset labels are slightly off but the intent is the BPB reserved-sector
  count), and **number of FATs** (offset 0x10). These are the FAT32 BIOS
  Parameter Block fields.

### `media_volume2` → sectors-per-FAT (lines 240–252)
```vhdl
when media_volume2 =>
    if(pkt_len = x"025") ... x"028") then
        secperfat <= die_rxdata & secperfat(31 downto 8);   -- 4 bytes, little-endian
        state_media <= media_volume3;
```
- **Offset 0x24–0x27** is the FAT32 32-bit "sectors per FAT" field
  (`BPB_FATSz32`). Note: this field **only exists in FAT32** — in FAT16 it's a
  16-bit field at offset 0x16. Reading it here on a FAT16 volume would grab
  garbage, which is the deeper reason the signature gate matters.

### `media_volume3` → compute the region map (lines 254–276)
```vhdl
when media_volume3 =>
    LBA_fatregion  <= LBA_fatregion_s;
    LBA_rootregion <= LBA_rootregion_s;
    LBA_dataregion <= LBA_dataregion_s;

-- the actual arithmetic (concurrent, lines 271–276):
LBA_fatregion_s  <= LBA_VOLUME0 + reserved_sec;                    -- FAT starts after reserved area
LBA_rootregion_s <= LBA_fatregion_s + secperfat + secperfat when num_fats = 2  -- skip 2 FAT copies
                    else LBA_fatregion_s + secperfat;             -- or 1
LBA_dataregion_s <= LBA_rootregion_s + secpercls;                 -- data starts after root
```
- **This is the standard FAT layout formula**, and being able to recite it is
  strong interview material:
  > "A FAT volume is laid out as: reserved sectors → FAT copies → root directory
  > → data. So FAT region = volume start + reserved; root region = FAT region +
  > (FATs × sectors-per-FAT); data region = root + root-dir-size. The code
  > computes exactly that."
- **`when num_fats = 2 ... else`** (line 273): only handles 1 or 2 FATs. Two is
  universal in practice, so fine — but technically another hardcoded assumption.

---

## Every limitation, now with a line number (interview cheat-sheet)

| Limitation | Exact location |
|---|---|
| FAT32 only (no NTFS/exFAT/FAT16) | Lines 217–226: `EB 58 90` signature gate |
| FAT32-specific secperfat field | Lines 242–248: reads 32-bit field at offset 0x24 |
| MBR only, no GPT | Lines 202–208: reads partition entry #1 at offset 0x1C6 |
| Partition #1 only | Same — only the first table entry is parsed |
| Assumes ≤2 FATs | Line 273: `when num_fats = 2 ... else` |
| 512-byte sectors assumed | Whole design: offsets are absolute byte positions in a 512-byte sector; bytes-per-sector (offset 0x0B) is never read |

**Framing for interview:** none of these are bugs — they're the natural
consequence of targeting FAT32 USB storage specifically. "The device is scoped
to FAT32 mass storage; I validate that explicitly and skip anything else rather
than mis-parsing it." That's a *defensible design decision*, and skipping-rather-
than-guessing is actually the safe choice.

## New patterns learned this phase
1. **Byte-offset parsing** driven by a position counter (`pkt_len`).
2. **Little-endian assembly** via shift-in (`data & reg(hi downto 8)`).
3. **Signature/magic-number validation** as a gate (`EB 58 90`).
4. **Multi-stage parse** across chained states (idle→mbr→volume1→2→3).
5. **On-disk-structure knowledge encoded as constants** — the FAT layout math.

---

### Next: Phase 5 — `usb2_bridge_rsrw_verilog.v`, the Verilog link-training block
(different language, the chirp handshake, and a vendor-authored file).

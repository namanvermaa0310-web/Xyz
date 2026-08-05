# USB Project — Line-by-Line Walkthrough
## Phase 3: The Sorting Brain (`usb_dism.vhd`)

> This is the module an interviewer will find most impressive, because it's
> where your project stops being "generic USB plumbing" and starts doing its
> actual clever job: recognizing mass-storage traffic and pulling out the disk
> sector address. Patterns from Phases 1–2 are assumed known.

### A correction I owe you (I was wrong twice)

In earlier summaries I said `usb_media_struct` (and its FAT-parsing children)
were **orphaned / disconnected.** **That was wrong.** `usb_dism` *instantiates*
`usb_media_struct` at line 1041 (`usb_media_struct_inst : usb_media_struct`).
I missed it because I traced the module tree top-down and stopped before
descending into `usb_dism`'s internals. So the FAT-parsing branch **is live**,
reached *through* `usb_dism`. Corrected everywhere now. (Lesson for you too:
always trace instantiations to the bottom before declaring something dead.)

---

## The one-line interview answer

> "`usb_dism` — 'dismantle' — is the traffic sorter. It watches the USB packet
> stream, separates bulk (mass-storage) traffic from everything else, and for
> read/write commands it parses the SCSI command wrapper to extract the LBA —
> the disk sector number being accessed — which the encryption stage needs as
> its address-dependent tweak."

---

## Entity (lines 11–48): what goes in and out

Inputs: the standard active/valid/data stream, for **both** sides
(`hie_` = host side, `die_` = device side).

Outputs — and this is the module's whole purpose, visible right in the pinout:
```vhdl
hie_rxactive_bulk / _valid_ / _data   -- the BULK traffic, split out
hie_rxactive_nb   / _valid_ / _data   -- the NON-BULK traffic, split out
...
LBAaddr_dism    : OUT ...(31 downto 0) -- THE EXTRACTED SECTOR ADDRESS
LBA_rootregion  : OUT ...(31 downto 0) -- filesystem root location (from the FAT parser)
```
- Two output streams per side (**bulk** and **nb** = non-bulk) + the extracted
  **LBA**. If you understand just the entity, you already understand what the
  module *does*: **sort into two piles, and emit the sector address.**

## Instantiated child (lines 56–93): `usb_media_struct`
The entity declares `usb_media_struct` as a component here and wires it up at
line 1041. That's the FAT-filesystem-awareness block (Phase 4). For now, know
that `usb_dism` feeds it the LBA and command type and gets back the
filesystem region addresses.

## Constants (lines 96–110): the USB packet-ID dictionary
```vhdl
constant pid_out  : ...:= "0001";   constant pid_in   : ...:= "1001";
constant pid_setup: ...:= "1101";   constant pid_ack  : ...:= "0010";
constant pid_nak  : ...:= "1010";   constant pid_stall: ...:= "1110";
constant pid_nyet : ...:= "0110";   constant pid_ping : ...:= "0100";
constant pid_sof  : ...:= "0101";   constant pid_data : ...:= "011";
```
- These name the **PID** (packet ID) values from the USB spec — the label byte
  at the front of every packet (Phase-1 background idea #3). `pid_out`/`pid_in`/
  `pid_setup` are token types; `pid_ack`/`pid_nak` are handshakes; `pid_sof` is
  "start of frame." Giving them names makes the state machine readable.
- **Interview:** you should be able to say "a USB packet starts with a PID that
  identifies its type — token, data, or handshake — and my sorter switches on
  that PID."

---

## The main sorting FSM (lines ~400–620): how it decides bulk vs non-bulk

This is a bigger version of the Phase-1 state machine — same structure (`case
state is ... when X => ... state <= Y`), just more states doing real protocol
work. Walking the important ones:

### `dism_idle` (lines 421–460) — classify the incoming token
```vhdl
when dism_idle =>
    s_in <= '0'; s_out <= '0'; s_setup <= '0'; ...    -- clear all the flags
    if(hie_rxvalid_dism = '1') then                    -- a packet's first byte arrived
        case hie_rxdata_dism(3 downto 0) is            -- look at its PID nibble
            when pid_setup => s_setup <= '1'; state_dism <= dism_gettoken_c1;
            when pid_in    => s_in    <= '1'; state_dism <= dism_gettoken_c1;
            when pid_out   => s_out   <= '1'; state_dism <= dism_gettoken_c1;
            when pid_sof   => s_sof   <= '1'; state_dism <= dism_gettoken_c1;
            when pid_ping  =>
                if HSSUPPORT then s_ping <= '1'; state_dism <= dism_gettoken_c1;
                else state_dism <= dism_skip;          -- PING only exists at High Speed
                end if;
            when others => state_dism <= dism_skip;    -- unknown PID → skip it
        end case;
    end if;
```
- **The core classification.** It reads the PID and sets a one-hot flag
  (`s_in`/`s_out`/`s_setup`/`s_sof`/`s_ping`) recording what kind of
  transaction this is, then moves on. Anything unrecognized → `dism_skip`.
- **`if HSSUPPORT ... else ... dism_skip`** (lines 450–455): PING is a
  High-Speed-only packet. On Full Speed there's no PING, so it's skipped. Good
  detail to mention — shows the design is speed-aware.

### `dism_skip` (lines 464–476) — the "not interesting" path
```vhdl
when dism_skip =>
    if(hie_rxvalid_dism = '0') then state_dism <= dism_idle; end if;
    s_in <= '0'; s_out <= '0'; ...                     -- clear flags, forward as non-bulk
```
- Traffic we don't care about (control transfers, unknown PIDs) waits out the
  packet and returns to idle. This traffic flows out the **non-bulk** stream
  untouched — it does NOT get encrypted. Important: only bulk read/write data
  is processed; everything else passes through clean.

### `dism_gettoken_c1` (lines 481–498) — token finished, route by type
```vhdl
when dism_gettoken_c1 =>
    if(hie_rxactive_dism = '0') then                   -- token packet ended
        if(s_sof = '1') then       state_dism <= dism_idle;         -- SOF: nothing to do
        elsif(s_setup='1' or s_out='1' or s_in='1') then state_dism <= dism_gottoken_c1;
        elsif(s_ping = '1') then   state_dism <= dism_waitack_c1;
        else state_dism <= dism_idle; end if;
    end if;
    if(HSSUPPORT and I_HIGHSPEED='1') then wait_count <= wait_timeout_hs;
    else wait_count <= wait_timeout_fs; end if;         -- arm a timeout, speed-dependent
```
- Routes based on the flag set earlier, and **arms a timeout counter**
  (`wait_count`) whose length depends on the negotiated speed. USB requires
  responses within bounded time; this counter enforces that so a missing reply
  can't hang the FSM. **Interview gold:** "it uses speed-dependent timeouts
  because HS and FS have different turnaround limits."

### `dism_gottoken_c1` (lines 500–551) — is this a DATA packet?
```vhdl
if((s_out='1' or s_setup='1') and (hie_rxdata_dism(2 downto 0) = pid_data)) then
    state_dism <= dism_getdata_c1;                     -- yes → go capture the data
else
    ... re-classify as a new token (same case as dism_idle) ...
elsif(die_rxactive_dism = '1') then
    state_dism <= dism_recvdata_con1;                  -- data coming from device (IN transfer)
elsif(wait_count = 0) then
    state_dism <= dism_idle;                           -- timed out → give up cleanly
end if;
wait_count <= wait_count - 1;                          -- tick the timeout down
```
- After an OUT/SETUP token, if a DATA packet follows, go capture it
  (`dism_getdata_c1`). Otherwise it either re-classifies a new token, handles
  device→host data, or **times out** to idle. The `wait_count - 1` every cycle
  is the countdown.

### `dism_getdata_c1` (lines 557–562) — capture the command payload
```vhdl
when dism_getdata_c1 =>
    if(hie_rxactive_dism = '0') then state_dism <= dism_gotdata_c1; end if;
    bulk_cmd_update <= '1';
```
- This is the state during which the **CBW (Command Block Wrapper)** bytes
  stream past — and while we're here, a *separate* process (below) is watching
  those same bytes to pull out the command type and LBA.

### `dism_gotdata_c1` (lines 566+) — decide if it was a read/write
```vhdl
when dism_gotdata_c1 =>
    if(bulk_command = '1') then                         -- the parser said "READ or WRITE"
        s_bulk_cmd <= '1';                              -- latch: this transfer is BULK
        state_dism <= dism_waitack_c1;
    ...
```
- `bulk_command` is set by the CBW parser (next section) only for READ(10)/
  WRITE(10). When true, this transfer is flagged **bulk** — meaning its data
  will be routed to the bulk output stream (and thus to encryption).

---

## The CBW parser (lines 885–969): where the LBA actually comes out

This runs in parallel with the FSM above, watching the same byte stream.

```vhdl
cmd_block_wrapper1 <= '1' when (cmdhdr = x"55534243") else '0';
```
- **`0x55534243` is "USBC" in ASCII** — the signature at the start of every USB
  Mass Storage **Command Block Wrapper**. This line detects "a mass-storage
  command is starting." (Line 993 does the same for `0x55534253` = "USBS", the
  status wrapper at the end.)
- **Interview:** "I detect the BOT command wrapper by its 'USBC' signature,
  then parse fields at fixed byte offsets."

```vhdl
HOST_Command_block:process (usb_clk_us)
begin if (rising_edge(usb_clk_us)) then
    bulk_command <= '0';
    if((state_dism = dism_getdata_c1) and (hie_rxvalid_dism = '1')) then
        case byte_pos is
            when x"000"..x"003" => cmdhdr <= cmdhdr(23 downto 0) & hie_rxdata_dism; -- signature
            when x"00F"         => cmdtype_s1 <= hie_rxdata_dism;                    -- SCSI opcode
            when x"011"..x"014" => cmdLBA <= cmdLBA(23 downto 0) & hie_rxdata_dism;  -- the 4 LBA bytes
            when x"016"..x"017" => cmdTransfer_len <= ... & hie_rxdata_dism;         -- transfer length
            when others => ...
        end case;
    elsif(state_dism = dism_gotdata_c1) then
        if(cmd_block_wrapper1 = '1' and host_pkt_len_s1 = x"022") then    -- valid 34-byte CBW
            LBAaddr1     <= cmdLBA;               -- ← LATCH THE EXTRACTED LBA
            cmdtype1     <= cmdtype_s1;
            Transfer_len <= cmdTransfer_len;
            if(cmdtype_s1 = x"28" or cmdtype_s1 = x"2A") then   -- 0x28=READ(10), 0x2A=WRITE(10)
                bulk_command <= '1';              -- ← only READ/WRITE get flagged bulk
            end if;
        end if;
    end if;
end if; end process;
```
- **This is the single most important block in the module.** Read it as: as
  each command byte streams by, grab the ones at the offsets we care about —
  the SCSI opcode at byte 0x0F, the 4-byte LBA at bytes 0x11–0x14, the transfer
  length at 0x16–0x17. The `& hie_rxdata_dism` trick **shifts each new byte into
  a register**, assembling multi-byte fields one byte at a time (same
  shift-in-a-byte idiom you saw in the FIFO packing).
- **The offsets are the SCSI/BOT spec.** In the 31-byte CBW: byte 15 (0x0F) is
  the SCSI command opcode, and for READ(10)/WRITE(10) the 4-byte big-endian LBA
  sits at CDB offset 2, which lands at these positions.
- **`0x28` / `0x2A`** — READ(10) and WRITE(10). *This one line is your entire
  "FAT/limited command support" story:* only these two opcodes are recognized.
  READ(12)/(16), WRITE(12)/(16) — ignored. (This is the concrete limitation we
  discussed; now you can point at the exact line.)
- **`host_pkt_len_s1 = x"022"` = 34 bytes** — a length sanity check (31-byte CBW
  + PID + CRC framing = 34) before trusting the parse. Good defensive coding to
  praise in an interview.

---

## How the sorted streams actually get routed (lines 273–301)

```vhdl
s_hie_rxactive_bulk <= s_hie_rxactive_dism when (state_dism = dism_getdata_d1) else '0';
```
- **Concurrent muxing:** the *same* input stream is steered to the bulk output
  only during the data-transfer states, and to the non-bulk output otherwise.
  So "sorting" isn't copying data into two buffers — it's **gating the one
  stream onto one of two output ports depending on FSM state.** Efficient, and
  worth explaining that way.

---

## Phase 3 summary (the interview centerpiece — say this)

> "`usb_dism` is a protocol-aware state machine that classifies every USB
> packet by its PID, routes bulk mass-storage data and non-bulk control traffic
> to separate outputs, and enforces speed-dependent timeouts so a missing
> response can't stall it. In parallel, a second process detects the BOT command
> wrapper by its 'USBC' signature and parses the SCSI command — extracting the
> opcode, the 32-bit LBA, and the transfer length at their fixed byte offsets.
> It only flags READ(10) and WRITE(10) as bulk, and hands the extracted LBA to
> the encryption stage as its address tweak. It also instantiates the FAT
> filesystem parser to locate the volume's regions."

## What this module reveals about the whole project (big-picture for interview)
- The device is a **transparent inline filter**: it sits mid-stream, and only
  *bulk read/write payload* is diverted for encryption; control traffic and
  unrecognized commands pass through untouched.
- Every limitation we found (READ(10)/WRITE(10) only, 32-bit LBA, BOT-only,
  512-byte assumption) traces to specific, pointable lines here and in the FAT
  parser — you can defend each as a scoping decision, not a bug.

## New patterns learned this phase
1. **Protocol FSM** switching on PID values.
2. **Shift-in-a-byte** to assemble multi-byte fields (`reg <= reg(...) & new_byte`).
3. **Parsing at fixed byte offsets** with a position counter.
4. **Signature detection** (`0x55534243` = "USBC").
5. **Speed-dependent timeouts** to prevent stalls.
6. **State-gated stream muxing** — routing without copying.
7. **Length sanity-checking** before trusting parsed data.

---

### Next: Phase 4 — `usb_dev_dir_detect.vhd`, the FAT32 boot-sector parser
(where the "FAT-only" limitation is born, line by line).

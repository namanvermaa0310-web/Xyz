PicoRV32 Minimal SoC - "Hello World" on a RISC-V core in ModelSim
=================================================================
This is your STEP 1 milestone, pre-built and already verified:
a RISC-V (RV32I) core executes a program and prints over a
memory-mapped output port. No board and no RISC-V toolchain
needed to run THIS - the program is already compiled (firmware.hex).

FILES
  picorv32.v    - the RISC-V core (unmodified, from YosysHQ/picorv32)
  tb_picosoc.v  - tiny SoC: core + 64KB RAM + print port + halt port
  firmware.hex  - the compiled program (loaded into RAM at sim start)
  run.do        - ModelSim script that compiles and runs everything
  firmware.c / start.S / link.ld - the SOURCE of firmware.hex, for when
                  you install the toolchain and start changing the program

HOW TO RUN (Windows + ModelSim)
  1. Put all these files in one folder.
  2. Open ModelSim.
  3. File > Change Directory... > select that folder.
  4. In the Transcript window at the bottom, type:   do run.do
  5. You should see in the transcript:
         Hello from PicoRV32!
         [sim] HALT - firmware finished.

  That's a RISC-V core running a compiled C program on your machine.

NOTE
  A "Not enough words in the file" warning on firmware.hex is normal and
  harmless - the program is just smaller than the 64KB RAM.

MEMORY MAP (how the print works)
  0x00000000..0x0000FFFF : RAM (program + data + stack)
  0x10000000             : write a byte here -> printed as a character
  0x10000004             : write here        -> ends the simulation

NEXT STEP (later, not now)
  Install a RISC-V GCC toolchain (xPack riscv-none-elf-gcc on Windows),
  then you can edit firmware.c, recompile, and run real programs - building
  up toward running ML-KEM software on this core.

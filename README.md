# gsu-host

Pico (RP2040) firmware that drives a real Super FX cartridge as a
stand-alone processor. It is not built or run yet: this machine had no
ARM toolchain or Pico SDK, and I have no hardware to test on. The C file
passes a syntax check against stub headers, and the GSU programs were run
on a small simulator of my own (opcode table from the Yoshi's Island
disassembly wiki), which checks logic and branch offsets but not hardware.

## Build

    export PICO_SDK_PATH=/path/to/pico-sdk
    mkdir build && cd build && cmake .. && make
    # copy gsu_host.uf2 to the Pico in BOOTSEL mode
    # demo program instead of the loader: cmake -DCMAKE_C_FLAGS=-DGSU_PROGRAM_DEMO=1 ..

## Wiring (5 V cart <-> 3.3 V Pico: level shifters required)

| Pico    | Cart signal |
|---------|-------------|
| GP0-9   | A0-A9       |
| GP10-17 | D0-D7       |
| GP18    | /RD         |
| GP19    | /WR         |
| GP20    | /RESET      |
| GP21    | (optional) 21.477 MHz clock, see ENABLE_GSU_CLK_OUT |

A10-A23 are hard-wired so the cart decodes $3000-$32FF in bank $00.
Cart ROM/RAM buses are never touched by the host. Power the cart from 5 V.
The host polls SFR.GO instead of using /IRQ, so /IRQ is not connected.

## Startup text, then binary

After reset the firmware prints a line with VCR and SFR, then `READY\r\n`.
Everything after `READY\r\n` is the raw byte stream of the GSU program's
read/write syscalls. A PC app should read lines until `READY`.

## RAM loader (default)

The firmware puts a 65-byte loader (padded to 80) in the GSU instruction cache and starts
it. Send, little endian, after `READY`:

| Bytes   | Meaning |
|---------|---------|
| 2       | load address in cart RAM (RAM bank 0 = $70) |
| 2       | entry address (jumped to after loading) |
| 2       | payload length, at least 1 |
| len     | payload |

The loader reads each byte with the read syscall, stores it with STB, then
runs LJMP to bank $70 at the entry address. The loaded program keeps using
the same syscalls through STOP. Notes:

- Assemble the payload for the address you load it at. Load and entry
  addresses are independent, so data can sit below the code.
- Length is limited by the cart's RAM size (commonly 32 KB or 64 KB), and
  the loader does not check it.
- Speed is set by the syscall round trip (about ten bus accesses per byte),
  roughly tens of KB/s. This is an estimate, not a measurement.
- The loader assumes RAMBR = 0 (bank $70) after reset and that SCMR gives
  the GSU the RAM (RAN/RON), which the firmware sets.

## System calls

The GSU program sets R1 (low byte) to the op and executes STOP. The host
sees GO cleared, services the call, and resumes by rewriting R15.

| R1 | Name   | R0 low byte                                              |
|----|--------|----------------------------------------------------------|
| 0  | read   | out: next byte from USB serial (blocks until available)  |
| 1  | write  | in: byte sent to USB serial                              |
| 2  | status | out: bit0 = RDRF (RX byte available), bit1 = TDRE (TX can accept a byte) |

The status bits follow the low two bits of a 6850 ACIA status register.
Bits 7..2 are returned as 0. TDRE is "USB connected and the CDC write FIFO
has room" (set USE_TUD_TX_STATUS to 0 to use "USB connected" only).

Resume rewrites R15 with the value it already holds (low byte, then high
byte; the high-byte write sets GO). This works for code at any address.

## STOP behaviour (from fullsnes, GSU-n chapter)

- STOP sets SFR.GO=0 and SFR.IRQ=1 (even with IRQ masked in CFGR) and leaves
  R15 = STOP address + 2. There is no delay slot: the byte after STOP is
  prefetched but not executed, so it is a dummy and resume skips it.
- Branches and jumps do have a delay slot: the byte after them executes
  before control moves to the target (the loader's LJMP is followed by NOP).
- STOP does not empty the code cache. The cache is cleared by CACHE, LJMP,
  or a host write of GO=0 to SFR (which also sets CBR=0). The firmware never
  writes SFR with GO=0 after loading the program.
- The "NOP before STOP" advice is for the MC1 (Star Fox cart): STOP can
  hang right after a RAM write there. The loader keeps several instructions
  between STB and the next STOP. GSU-2 is not affected. Use a GSU-1/2 cart.

## Demo program

With `-DGSU_PROGRAM_DEMO=1`, the GSU program waits until RDRF is set
(status syscall), reads a byte, adds 1, and writes it back, so each
character you type returns as character + 1.

## Still to verify on hardware

1. A10-A23 hard-wiring selects the GSU register window.
2. Where the GSU gets its clock (cart oscillator vs. console).
3. Writing R15's high byte sets GO.
4. A cache line becomes valid when its 16th byte is written, and code at
   R15=$0000 runs from the cache with CBR=0 and no ROM access.
5. R15 = STOP + 2 on a real cart (fullsnes flags this value with a
   question mark before confirming it).
6. Hand-assembled programs. Opcodes are from the table above, but these are
   not confirmed against an assembler: prefix combinations such as
   `TO Rd; FROM R0; LOB`, `FROM R4; ALT1; LJMP R8`, and the branch offsets,
   which assume offsets count from the byte after the branch operand.
7. LJMP: PBR = R8 low byte, R15 = Sreg, and the byte after it executes from
   the old cache before the jump.
8. BUS_DELAY_CYCLES is long enough for the cart's register timing.
9. TinyUSB's tusb.h is reachable from this target; if the build complains,
   set USE_TUD_TX_STATUS to 0.

## Sources

- Fullsnes (Nocash SNES Specs), SNES Cart GSU-n chapters: https://problemkaputt.de/fullsnes.htm
- Super FX instruction set table (Yoshi's Island disassembly wiki): https://github.com/brunovalads/yoshisisland-disassembly/wiki/Super-FX-Instruction-Set
- rmstdope/neser PR 3205 (cache clearing conditions): https://github.com/rmstdope/neser/pull/3205
- sp00nznet/LakeSnes PR 2 (byte after a jump runs first): https://github.com/sp00nznet/LakeSnes/pull/2

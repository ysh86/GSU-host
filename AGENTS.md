# AGENTS.md — gsu-host

Notes for AI agents (and humans) picking this project up. `README.md` is the
user-facing doc (wiring, protocol, usage); this file covers how to work on it.

## What this is

RP2040 (Pico 1) firmware that acts as the S-CPU for a real Super FX (GSU)
cartridge, using the GSU as a stand-alone processor. The Pico touches only
the GSU register window ($3000-$32FF). GSU programs run from the instruction
cache or cart RAM and use STOP as a system call into USB serial.

## Layout

| Path                  | Purpose |
|-----------------------|---------|
| `gsu_host.c`          | all firmware: bus layer, GSU programs (byte arrays), syscall loop |
| `CMakeLists.txt`      | Pico SDK build (needs `PICO_SDK_PATH`) |
| `README.md`           | wiring, loader protocol, syscall table, verification status, optimization ideas |
| `docs/cache-and-chip-notes.ja.md` | Japanese notes: GSU cache behaviour and GSU-1 vs MC1 measurements |
| `tools/check.sh`      | syntax check (both program modes) + simulator run |
| `tools/gsusim.py`     | mini GSU simulator; reads `loader_prog` / `demo_prog` from `gsu_host.c` |
| `tools/hangtest/`     | `make.py` builds `hang_test.load` (STB/STW-then-STOP test, load at $0200); checked in `gsusim.py` |
| `tools/stubs/`        | stub Pico SDK headers, only for the syntax check |

## Status: confirmed on a GSU-1 and an MC1, including the loader

The authoring sandbox had no ARM toolchain, no Pico SDK and no cartridge; the
user builds and tests on hardware. Confirmed on a real GSU-1 (board 1CA6B-01):
the `-DGSU_DIAG=1` build passes all checks (bus read/write, SFR flags,
R0-R13, latched writes, cache, start via R15, STOP with R15 = STOP + 2, resume
by rewriting R15, at CFGR/CLSR 80/01 and 00/00); the echo demo works (typed
characters return +1), so the read/write/status syscalls work; and the
measurement build gave 12.4 us and about 80,900 syscalls/s (bus read 1686 ns,
write 1712 ns, BUS_DELAY_CYCLES 64 at 125 MHz; 7 bus accesses per syscall).
The RAM loader and LJMP also work: the user loaded a binary build of the echo
program and it ran. `tools/check.sh` proves the
C parses and that the programs behave on a simulator written from reading the
docs; it does not prove hardware behaviour. No `[VERIFY]` markers remain in
the code; if you add something that has not been run on hardware, mark it
`[VERIFY]` and list it in the README verification status.

MC1 (MARIO chip, board 1C0N, VCR=01, external clock and /ROMSEL): the
diagnostic build passes the basics at both clocks and 21 MHz works (clock check
ratio 2.00). Host-written cache code is used only inside the block that ran
last (diag [7] confirmed; entering another block reads ROM). First seen as "only reliable in line 0": diag [6] shows
starts in other lines, or after writing two lines, execute ROM bytes; the
32-byte echo demo fails on its second pass (`BAD OP FF R15=0039`). Cause
unknown. `-DGSU_BOOT_STAGE1=1` is the workaround: 15-byte `boot_prog` in line 0
pulls `loader_prog` (served by the firmware) into RAM and LJMPs to $70:0000.
Confirmed on the MC1 at 10.7 MHz: the echo binary loaded through it and ran. At 21 MHz
the loader build ran away (TIMEOUT, SFR=0020), so use `-DGSU_MC1=1` (two-stage
boot + 10.7 MHz). Do not add cache programs
longer than 16 bytes for the MC1. In that mode the loader occupies RAM
$0000-$004F, so payloads must load at $0050 or above. `-DGSU_CLOCK_21MHZ=0` selects 10.7 MHz.

## Rules for changes

0. Project convention for loadable programs and test binaries: load and entry
   address `$0200`; `$0000-$01FF` is scratch RAM for variables (LMS/SMS reach
   it with 2-byte instructions) and its contents at start are undefined.
   Examples and new tools use this unless there is a reason not to.
1. Run `tools/check.sh` after every edit to `gsu_host.c`.
2. `loader_prog` and `demo_prog` are hand-assembled. The comment above each
   array, the bytes, and the README must agree. The simulator reads the
   bytes from the C file, so a wrong byte shows up there, but a wrong
   comment does not.
3. Take opcodes from the table in the Yoshi's Island disassembly wiki (URL
   in README), not from memory. A memory-based BEQ (0x0A, actually BPL)
   already broke the demo once. BNE = 0x08, BEQ = 0x09, BRA = 0x05.
4. Extend the simulator when you use a new opcode; it raises on any
   opcode it does not model.
5. Keep the user's conversation language in mind: the user writes Japanese.
   Code, comments and docs here are English.

## GSU facts that are easy to get wrong (sources in README)

- STOP clears GO, sets IRQ, leaves R15 = STOP + 2. The byte after STOP is
  prefetched, not executed: it is a dummy, and resume skips it. No NOP is
  needed after STOP. (NOP before STOP is an MC1-only workaround.)
- Branches and jumps (including LJMP) execute the byte after them first.
  Offsets are assumed to count from the byte after the operand.
- IBT sign-extends its 8-bit immediate (IBT R3,#99 gives R3 = FF99).
- STOP does not empty the code cache. A host write of GO=0 to SFR does (and
  sets CBR=0), so never do that after loading code. CACHE and LJMP also
  empty it.
- Writing R15's high byte starts the GSU. A cache line becomes valid when
  its 16th byte is written.
- Host writes to R0-R15 are latched: the even-address (LSB) write only sets
  LATCH, the odd-address (MSB) write applies LSB=LATCH, MSB=data. Never write
  an LSB alone and expect the register to change; use `gsu_reg16_write()`.
- While GO=1 the host may access only SFR, SCMR and VCR (fullsnes); it polls
  SFR and touches other registers after STOP.
- Cache loading: write SFR=0000h first (forces CBR=0, marks all lines empty),
  then write 16-byte lines; the last byte of a line marks it valid.
  `load_program()` does this, which is the one intended GO=0 write.

## Design decisions to keep

- Resume rewrites R15 with the value it already holds (low byte, then high
  byte). This is deliberately the most general and safest method, works for
  code at any address, and the user chose it over a single high-byte write
  that would save a few bus accesses. Do not "optimize" it without asking.
- Completion is detected by polling SFR.GO, not /IRQ.
- Syscalls are R1 low byte = op, R0 low byte = data: 0 read, 1 write,
  2 status (bit0 RDRF, bit1 TDRE, like a 6850 ACIA's low status bits). Read
  and status results are written to R0 as zero-extended 16-bit values.
- The default program is the RAM loader; `-DGSU_PROGRAM_DEMO=1` selects the
  +1 echo demo; `-DGSU_DIAG=1` (wins over all; PASS/FAIL checks, text only; use it first on any new wiring);
  `-DGSU_MEASURE=1` (wins over the others) selects the
  throughput measurement build (syscall 3 = NOP, report printed once, no
  prints during the timed loop). Output after the `READY\r\n` line is raw binary, so do not
  add debug prints after it.

## Not done yet

- The echo demo was cross-checked with bass (bytes match, runs on both chips);
  `loader_prog` and `boot_prog` were not, but they run on the cart.
- The README open questions (causes unknown, no action planned). Speed work (BUS_DELAY_CYCLES tuning, loader timing, block
  syscalls) is only noted under "Optimization ideas" in the README.
- Pin handling in the README wiring section is from the SNESdev wiki
  pinout, overridden by the user's PCB inspection of board 1CA6B-01. The
  two 8-pin expansion groups, pins 1-4/32-35 and 28-31/59-62 (SYSTEM CLK,
  EXPAND, PA0-PA7, /PRD, /PWR, /WRAMSEL, REFRESH, audio L/R), are confirmed
  unused and unconnected, so all 16 stay open. PHI2, A23 and /CART are
  unconnected; the CIC is not connected to the Super FX; the clock
  oscillator is on the cart. No pin is left unverified, and the fixed A10-A22 levels are confirmed to
  select the GSU registers. Other boards may differ. The chip on 1CA6B-01 is
  a GSU-1 (user-reported), so it is not the MC1. The
  MC1 hang reported for an STB immediately followed by STOP was reproduced with `tools/hangtest/` (MC1 hangs with 0 or 1 NOP between the write and STOP, 2 or more are fine; GSU-1 no hang); the loader and stage 1 keep
  several instructions between STB and the next STOP.
- PC-side sender for the loader protocol (the user is writing their own app).

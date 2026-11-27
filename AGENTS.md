# AGENTS.md — GSU-host (v1.3.1)

Notes for AI agents (and humans) picking this project up. `README.md` is the
user-facing doc (wiring, protocol, usage); this file covers how to work on it.

## What this is

RP2040 (Pico 1) firmware that acts as the S-CPU for a real Super FX (GSU)
cartridge, using the GSU as a stand-alone processor. The Pico drives the GSU
register window ($3000-$32FF) and, for the debug monitor, reads the cart RAM
window ($6000-$7FFF) while the GSU is stopped (A10-A14 are on GP21-GP25). The
loader always runs from cart RAM ($0000-$004F, entered through a 15-byte stage 1
in the cache); programs run from cart RAM and the cache and use STOP as a
system call into USB serial (0 nop, 1 exit, 3 read, 4 write, 28 fstat; 43 times is reserved).

## Layout

| Path                  | Purpose |
|-----------------------|---------|
| `gsu_host.c`          | all firmware: bus layer, GSU programs (byte arrays), syscall loop |
| `CMakeLists.txt`      | Pico SDK build (needs `PICO_SDK_PATH`) |
| `README.md`           | wiring, loader protocol, syscall table, verification status, optimization ideas |
| `CHANGELOG.md`        | release notes from 1.0.0; add an entry for every version |
| `docs/cache-and-chip-notes.ja.md` | Japanese notes: GSU cache behaviour and GSU-1 vs MC1 measurements |

## Status: confirmed on a GSU-1 and an MC1, including the loader

The authoring sandbox had no ARM toolchain, no Pico SDK and no cartridge; the
user builds and tests on hardware. Confirmed on a real GSU-1 (board 1CA6B-01):
the `-DGSU_DIAG=1` build passes all checks (bus read/write, SFR flags,
R0-R13, latched writes, cache, start via R15, STOP with R15 = STOP + 2, resume
by rewriting R15, at CFGR/CLSR 80/01 and 00/00); the echo demo works (typed
characters return +1), so the read/write/fstat syscalls work; and the
measurement build gave 12.4 us and about 80,900 syscalls/s (bus read 1686 ns,
write 1712 ns, BUS_DELAY_CYCLES 64 at 125 MHz; 7 bus accesses per syscall).
The RAM loader and LJMP also work: the user loaded a binary build of the echo
program and it ran. Nothing here is tested without the cart: the authoring sandbox only checked that the
C parses, which does not prove hardware behaviour. `[VERIFY]` marks what is not
confirmed on hardware: the effect of `c` / `m` on the clock (the monitor and the loader are confirmed on the GSU-1 and the MC1;
the GSU-2 was not run with the monitor and is assumed like the GSU-1; listed under "Still to verify" in the README). The A10-A14 pins
and the RAM window (host write/readback) are confirmed on the GSU-1 and GSU-2,
but not on the MC1: the S-CPU window neither reads nor writes RAM there (diag
[8].2-.5), so the monitor's `d` is disabled when VCR=01 (MC1). Likely
the MC1 maps RAM only at $70-$71 (untested). Diag [8].1 cannot tell a pulled-low
bus from a driving cart. Mark anything new the same way and list it there.

MC1 (MARIO chip, board 1C0N, VCR=01, external clock and /ROMSEL): the
diagnostic build passes the basics at both clocks and 21 MHz works (clock check
ratio 2.00). Host-written cache code is used only inside the block that ran
last (diag [7] confirmed; entering another block reads ROM). First seen as "only reliable in line 0": diag [6] shows
starts in other lines, or after writing two lines, execute ROM bytes; the
32-byte echo demo fails on its second pass (`BAD OP FF R15=0039`). Cause
unknown. The workaround is now the only boot path on every chip: 15-byte
`boot_prog` in line 0 pulls `loader_prog` (served by the firmware) into RAM and
LJMPs to $70:0000. Confirmed on the MC1 at 10.7 MHz: the echo binary loaded
through it and ran. At 21 MHz the loader build ran away (TIMEOUT, SFR=0020), so
use the default 10.7 MHz (monitor `c 0`). Do not add cache programs longer than 16 bytes for
the MC1. The loader occupies RAM $0000-$004F, so payloads must load at $0050 or
above (convention: $0200). The clock is chosen in the monitor (`c 0|1`, default 10.7 MHz).

## Rules for changes

0. Project convention for loadable programs and test binaries: load and entry
   address `$0200`; `$0050-$01FF` is scratch RAM for variables (LMS/SMS reach
   it with 2-byte instructions) and its contents at start are undefined.
   `$0000-$004F` holds the loader and must not be written: on the MC1, `l`
   after a program has run restarts it from there (`gsu_boot(true)`).
   Examples and new tools use this unless there is a reason not to.
1. `loader_prog`, `boot_prog` and `measure_prog` are hand-assembled. The comment above each
   array, the bytes, and the README must agree.
2. Take opcodes from the table in the Yoshi's Island disassembly wiki (URL
   in README), not from memory. A memory-based BEQ (0x0A, actually BPL)
   already broke the demo once. BNE = 0x08, BEQ = 0x09, BRA = 0x05.
3. Keep the user's conversation language in mind: the user writes Japanese.
   Code, comments and docs here are English.

## GSU facts that are easy to get wrong (sources in README)

- On the MC1, tight loops that mix multiplies with other instructions (16 x MULT;NOP,
  32 x FROM;ALT1;LMULT) hung the chip, while runs of identical multiplies were fine; the
  GSU-1A is not affected (README "Multiplier speed" finding 5). Unexplained.

- Programs loaded by the loader must start with `CACHE` ($02): the loader's LJMP leaves
  CBR=$0040 (window $0040-$023F), so without it a program at $0200 is cached for 64
  bytes only and runs at ROM/RAM speed after that (measured on the GSU-1A and MC1).

- On the MC1, a program that writes R14 (the ROM address pointer) went wrong after
  the first syscall (it ran code of the previously loaded program). Keep R14 out of
  programs that must run on the MC1; use R12/R13 or RAM (README, MC1 section).

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
- Completion is detected by polling SFR.GO, not /IRQ. There is no timeout by default: long-running programs are normal and a TIMEOUT message would corrupt the binary stream (it did, in the multiplier benchmark). `-DGSU_WATCHDOG_MS=500` turns the message on for hang hunting.
- Syscalls are R3 low byte = op, R0 low byte = data, numbered like Unix where
  there is a counterpart (i386 numbering): 0 nop, 1 exit (enters the monitor: d dump
  RAM, r regs, g resume, t timed resume, l load), 3 read, 4 write, 28 fstat (bit0
  RDRF, bit1 TDRE, like a 6850 ACIA's low status bits), 43 times (reserved, BAD OP). R3 is therefore reserved for the syscall
  number in GSU programs (the loader keeps its RAM pointer in R1; the hang test
  and multiplier benchmark use R5). Read
  and fstat results are written to R0 as zero-extended 16-bit values.
- A10-A14 are Pico outputs (GP21-GP25): AH_REGS (register window) all the time,
  AH_RAM only inside `ram_read()`, which also sets SCMR.RAN=0 and restores it.
  The firmware has no clock output (GP21 was the optional clock pin; the
  RP2040's clock output pins are the A10-A14 lines now).
- The clock and CFGR bit 5 (MS0, multiplier speed) are runtime settings of the
  monitor (`c 0|1`, `m 0|1`; defaults 10.7 MHz and standard), applied in
  `gsu_boot()` (always) and at the monitor's `g` (`gsu_apply_config(false)`, only
  what changed; the clock effect is not measured); `c 1` and `m 1` refuse each other. The MC1 is detected from
  VCR=01 (only used to disable `d` and warn on `c 1`). There are no
  `GSU_MC1`, `GSU_CLOCK_21MHZ` or `GSU_MS0` defines any more. The diagnostic build
  uses CLSR=01 and CFGR=80 itself. Measured (README "Multiplier speed"): on the GSU-1A at
  10.7 MHz it makes UMULT/FMULT/LMULT 1/4/4 cycles faster from the cache (FMULT 747 -> 374 ns);
  the MC1 seems to ignore it (measured uncached only); the GSU-2 is assumed like the GSU-1A (not measured).
  From the cache the multiplier scales with the clock: 21.4 MHz (MS0=0) takes half the time, so use `c 1`.
- The loader build starts in the monitor after reset and loads only on `l`
  (`monitor(false)`, then `gsu_boot()`); the measure build starts the GSU
  directly and runs from the cache. Loading is always two-stage (`gsu_boot()`).
- The default program is the stage-1 boot into the RAM loader; `-DGSU_DIAG=1` (wins over all; PASS/FAIL checks, text only; use it first on any new wiring);
  `-DGSU_MEASURE=1` (wins over the others) selects the
  throughput measurement build (syscall 0 = NOP, report printed once, no
  prints during the timed loop). Output after the `READY\r\n` line is raw binary, so do not
  add debug prints after it.

## Not done yet

- The echo demo was cross-checked with bass (bytes match, runs on both chips);
  `loader_prog` and `boot_prog` were not, but they run on the cart.
- The README open questions (causes unknown, no action planned). Speed work (BUS_DELAY_CYCLES tuning, loader timing, block
  syscalls) is only noted under "Optimization ideas" in the README.
- Pin handling in the README wiring section is from the SNESdev wiki pinout,
  overridden by the user's PCB inspection. Three boards are recorded in the
  README ("Boards and chips"): 1C0N (MC1; external clock; /ROMSEL connected;
  A23 not), 1CA6B-01 (GSU-1A; A23 and /ROMSEL not connected) and 1CB5B-01
  (GSU-2; A23 connected). One wiring serves all: A23 low, /ROMSEL high,
  A15-A22 low. The 16 expansion-terminal pins are unconnected on all three boards (SYSTEM
  CLK on 1C0N excepted); PHI2 is unconnected on all three; every cart has a CIC but it is not connected to the Super FX, so the CIC pins stay open. GSU-2
  (VCR=04) also worked with A23 open; tie it to GND anyway. The MC1 hang reported for an STB
  immediately followed by STOP was reproduced on the MC1 (hangs
  with 0 or 1 NOP between the write and STOP, 2 or more are fine; GSU-1 no
  hang); the loader and stage 1 keep several instructions between STB and the
  next STOP.
- The debug monitor is minimal (d, r, g, l). More commands (poke, breakpoints,
  step) are the user's next step; `ram_write()` exists only in the diagnostic
  build.
- PC-side sender for the loader protocol (the user is writing their own app).

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
| `README.md`           | wiring, loader protocol, syscall table, hardware TODO list |
| `tools/check.sh`      | syntax check (both program modes) + simulator run |
| `tools/gsusim.py`     | mini GSU simulator; reads `loader_prog` / `demo_prog` from `gsu_host.c` |
| `tools/stubs/`        | stub Pico SDK headers, only for the syntax check |

## Status: never built, never run on hardware

The authoring sandbox had no ARM toolchain, no Pico SDK, and no cartridge.
`tools/check.sh` is the only verification: it proves the C parses and that
the GSU programs behave on a simulator written from my own reading of the
docs. It does not prove hardware behaviour. Do not describe anything as
working on hardware. Keep the `[VERIFY]` markers in code and the "Still to
verify" list in the README until someone tests on a real cart.

## Rules for changes

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
- STOP does not empty the code cache. A host write of GO=0 to SFR does (and
  sets CBR=0), so never do that after loading code. CACHE and LJMP also
  empty it.
- Writing R15's high byte starts the GSU. A cache line becomes valid when
  its 16th byte is written.
- While GO=1, the host only polls SFR; it touches other registers after STOP.

## Design decisions to keep

- Resume rewrites R15 with the value it already holds (low byte, then high
  byte). This is deliberately the most general and safest method, works for
  code at any address, and the user chose it over a single high-byte write
  that would save a few bus accesses. Do not "optimize" it without asking.
- Completion is detected by polling SFR.GO, not /IRQ.
- Syscalls are R1 low byte = op, R0 low byte = data: 0 read, 1 write,
  2 status (bit0 RDRF, bit1 TDRE, like a 6850 ACIA's low status bits).
- The default program is the RAM loader; `-DGSU_PROGRAM_DEMO=1` selects the
  +1 echo demo. Output after the `READY\r\n` line is raw binary, so do not
  add debug prints after it.

## Not done yet

- Real GSU assembler pass over the hand-assembled programs (bass or WLA-DX).
- Build with the Pico SDK; check `tusb.h` is reachable (fallback:
  `USE_TUD_TX_STATUS=0`).
- Everything in the README "Still to verify on hardware" list.
- Pin handling in the README wiring section comes from the SNESdev wiki
  pinout only. PHI2 (57), REFRESH (33), the CIC pins and whether the GSU
  clock comes from SYSTEM CLK (pin 1) are unverified; check a GSU cart
  schematic before wiring.
- PC-side sender for the loader protocol (the user is writing their own app).

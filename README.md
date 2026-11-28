# GSU-host v1.4.0

Pico (RP2040) firmware that drives a real Super FX cartridge as a
stand-alone processor.

Status: confirmed on a real GSU-1 cart (board 1CA6B-01) and on a MARIO chip
(MC1) cart (board 1C0N); the diagnostic build also passes everything on a
GSU-2 cart. The MC1 differs; see the MC1 section.

- Diagnostic build (`-DGSU_DIAG=1`): bus reads and writes, SFR flag bits,
  R0-R13 and the latched-write rule, cache write/readback, start (GO via the
  R15 high byte), STOP with R15 = STOP + 2, and resume by rewriting R15, at
  CFGR/CLSR = 80/01 (21 MHz) and 00/00. All checks pass.
- Echo demo: each typed character returns +1, so the fstat, read and write
  syscalls work end to end over USB serial (earlier as a cache-resident build,
  now sent through the loader).
- Measurement build (`-DGSU_MEASURE=1`): see "Measuring throughput" for the
  numbers (about 12.4 us and 80,900 syscalls/s).
- RAM loader: the user loaded a binary build of the echo program through it
  and it ran (`Hello,world` came back as `Ifmmp-xpsme`). This confirms the
  loader's hand-assembled code, STB to cart RAM, LJMP to bank $70 and the
  loaded program's syscalls. On the MC1 this worked through a two-stage boot
  (stage 1 in the cache, loader from cart RAM); the cache-resident echo demo
  (32 bytes) does not work on the MC1. Every chip now uses that two-stage
  boot (see "RAM loader"); it now also runs on the GSU-1 (VCR=04).
- Debug monitor: the start-up monitor with `l`, `i`, `c` and `m` works on the
  GSU-1 (VCR=04) and the MC1. The exit syscall, `r`, `g`, `t` and `l` after an
  exit work on both; `d` (RAM window read through A10-A14 on GP21-GP25) works on
  the GSU-1 (the dump shows the loader at $0000-$004F). The GSU-2 has not been
  run with the monitor yet (see "Verification status").
- The programs were also checked on a small simulator, which checks logic and
  branch offsets, not hardware.

VCR reads 04 on the GSU-1 cart and 01 on the MC1 cart. Documented values are 1 (MC1) and 4 (GSU-2); a
GSU-1 is not listed there, and web reports give 01 or 04 for it.

Measurements and notes on the code cache and the GSU-1 / MC1 differences (in
Japanese): `docs/cache-and-chip-notes.ja.md`.

## Build

    export PICO_SDK_PATH=/path/to/pico-sdk
    mkdir build && cd build && cmake .. && make
    # copy gsu_host.uf2 to the Pico in BOOTSEL mode
    # hang watchdog (prints TIMEOUT every 500 ms while the GSU runs; off by default): -DGSU_WATCHDOG_MS=500
    # clock and multiplier speed are set at run time in the monitor: c 0|1, m 0|1 (see "Monitor")

## MARIO chip (MC1, VCR=01)

Tested on a Japanese Star Fox cart (MC1, board 1C0N) with SYSTEM CLK supplied by
an external oscillator and /ROMSEL tied high. Results of the diagnostic build on this cart:

- Bus, registers, latched writes, cache readback, start/STOP/resume from line 0
  all pass, at CLSR=00 and CLSR=01.
- The clock check ([5]) measured a slow/fast time ratio of 2.00 (24,426 us vs
  12,211 us for 65535 iterations), so CLSR=01 gives the 21 MHz mode on this
  chip, despite forum reports that the MC1 does not support it.
- After STOP, SFR reads 8000 (IRQ flag still set); the GSU-1 read 0000 at the
  same point. The firmware clears it by reading SFR high.
- **Host-written cache code is only used inside the block that ran last** (diag
  [7] confirmed it: the same block-0 program failed right after a run that ended
  in block 1 and passed on the next try). Entering another block, by start, branch or
  falling through, reads ROM instead. See `docs/cache-and-chip-notes.ja.md`. Observed first as: code in line 0 ($00-$0F) works,
  other lines do not. Programs in
  line 1 or 31, and any run that started after writing two lines, executed
  other bytes (ROM data) at the first fetch ([6] tests FAIL). The echo demo
  (32 bytes, two lines) echoed the first character, then ran off into ROM and
  ended in `BAD OP FF R15=0039` at both 21 and 10.7 MHz. The cause is not
  understood: resuming at $12 inside a host-written line 1 did execute the
  host's bytes.

- **Do not write R14 in programs for the MC1.** A Mandelbrot-style program (146
  bytes, loaded at `$0200`) that used R14 for an intermediate value ran
  correctly on the GSU-1, but on the MC1 it went wrong after the first
  syscall: it ran code of the program loaded before it (the echo program) and
  ended in `EXIT` or `BAD OP` at addresses that belong to that program. Keeping the
  intermediate value in RAM (STW / LDW) instead of R14 made it run correctly on
  the MC1, at a small cost in speed. R14 is the ROM address pointer, which
  the documentation says is used for ROM access (a write to R14 starts a ROM
  read); the program touches no ROM, so the effect is unexplained. It may be
  another face of the MC1's cache behaviour above. `[VERIFY]` Programs that
  must run on both chips should leave R14 alone (R12/R13 and RAM are free).

**Use 10.7 MHz on the MC1.** At 21 MHz the echo loader build ran away (no
STOP ever reached; a `-DGSU_WATCHDOG_MS=500` build printed `TIMEOUT: GSU still running, SFR=0020`), although the short
diagnostic tests and the clock check pass at 21 MHz. The cause is not known; the
MC1's rated clock is 10.7 MHz.

Use the default 10.7 MHz clock on the MC1 (monitor `c 0`; the firmware detects the MC1 from VCR=01, which makes `d` read through bank $70). The
loader itself is the same on every chip: because the MC1 only runs host-written
code from one cache block, the loader runs from cart RAM (see "RAM loader").
Confirmed on the MC1 at 10.7 MHz: loading the echo binary works (`Hello,world`
came back as `Ifmmp-xpsme`), so the loader, LJMP to cart RAM, and execution
from RAM with the normal cache fill all work there.

## Wiring (5 V cart <-> 3.3 V Pico: level shifters required)

Pin numbers are the 62-pin SNES cartridge connector, from the SNESdev wiki
(link in Sources). Directions there are read from a diagram, so treat them as
approximate. Every connector pin is accounted for below.

**Boards and chips** (from the user's inspection of three carts)

| Cart | PCB | Chip | External clock | A23 (pin 48) | /ROMSEL (pin 49) |
|------|-----|------|----------------|--------------|------------------|
| Star Fox (JP) | 1C0N | MARIO chip 1 (MC1, VCR=01) | needed (on SYSTEM CLK, pin 1) | not connected | connected |
| Wild Trax (JP) | 1CA6B-01 | GSU-1A (VCR=04) | not needed | not connected | not connected |
| Yoshi's Island (JP) | 1CB5B-01 | GSU-2 (VCR=04) | not needed | connected | not connected |

(The GSU-1A is called GSU-1 elsewhere in this README.) One wiring serves all
three: **A23 low (GND), /ROMSEL high (idle level of GP27)**, the rest as in the pin map. On a board
that does not connect one of those pins, tying it is harmless. The GSU-2 cart
also worked with A23 left open, but a connected input should not float, so tie
it to GND. On 1C0N the clock
comes from an external 21.477 MHz oscillator module (see below); the other two
have the oscillator on the cart.

On all three boards the 16 pins of the two 8-pin groups at the cart's two ends
(the "expansion terminals") are unused and unconnected, except SYSTEM CLK
(pin 1) on 1C0N. They are pins 1, 2, 3, 4, 32, 33, 34, 35 and pins 28, 29, 30,
31, 59, 60, 61, 62: SYSTEM CLK, EXPAND, PA0-PA7, /PRD, /PWR, /WRAMSEL, REFRESH
and the left/right audio pins. Leave all 16 open (on 1C0N, pin 1 takes the
external clock).

Also on all three boards:

- PHI2 (CPU clock, pin 57) is not connected.
- Every cart has a CIC (the lock-out chip, connector pins 24, 25, 55, 56), but it
  is not connected to the Super FX, and the carts run with those pins open, so
  it plays no part here.

Consequently the GSU clock comes from the oscillator on the cart (or from the
external module on 1C0N), not from the Pico. The cart runs from power plus the
signals in the first two tables.

**Pin map**

All 62 connector pins and how this project treats each one. Each row shows
two pins that face each other across the card edge: pin i on one side
(1-31, left column) and pin i+31 on the other (32-62, right column). Signal
names and the `dir` column come from the SNESdev wiki table (in = to the cart, out = to the
console, bi = bidirectional; the directions are read from a diagram, so treat
them as approximate). The `role` column is this project's decision, as in
the tables below.

```
 # signal     dir role  |  # signal     dir role
------------------------+------------------------
 1 SYSTEM CLK in  open* | 32 /WRAMSEL   in  open*
 2 EXPAND     bi  open* | 33 REFRESH    in  open*
 3 PA6        in  open* | 34 PA7        in  open*
 4 /PRD       in  open* | 35 /PWR       in  open*
 5 GND        --  GND   | 36 GND        --  GND
 6 A11        in  GP22  | 37 A12        in  GP23
 7 A10        in  GP21  | 38 A13        in  GP24
 8 A9         in  GP9   | 39 A14        in  GP25
 9 A8         in  GP8   | 40 A15        in  L
10 A7         in  GP7   | 41 A16        in  L
11 A6         in  GP6   | 42 A17        in  L
12 A5         in  GP5   | 43 A18        in  L
13 A4         in  GP4   | 44 A19        in  L
14 A3         in  GP3   | 45 A20        in  L
15 A2         in  GP2   | 46 A21        in  L
16 A1         in  GP1   | 47 A22        in  L
17 A0         in  GP0   | 48 A23        in  L   
18 /IRQ       out open  | 49 /ROMSEL    in  H
19 D0         bi  GP10  | 50 D4         bi  GP14
20 D1         bi  GP11  | 51 D5         bi  GP15
21 D2         bi  GP12  | 52 D6         bi  GP16
22 D3         bi  GP13  | 53 D7         bi  GP17
23 /RD        in  GP18  | 54 /WR        in  GP19
24 CIC data 1 bi  open  | 55 CIC data 2 bi  open
25 CIC reset  in  open  | 56 CIC CLK    out open
26 /RESET     bi  GP20  | 57 PHI2       in  open
27 +5V        --  +5V   | 58 +5V        --  +5V
28 PA0        in  open* | 59 PA1        in  open*
29 PA2        in  open* | 60 PA3        in  open*
30 PA4        in  open* | 61 PA5        in  open*
31 Audio L    out open* | 62 Audio R    out open*
```

| role | meaning |
|------|---------|
| GPn  | driven by Pico GPIO n, through a level shifter |
| H, L | hard-wired high / low |
| open | leave open (not needed, or not connected on the inspected carts) |
| open* | one of the 16 expansion-terminal pins: unused and unconnected (SYSTEM CLK on 1C0N excepted), leave open |
| +5V, GND | power |

SYSTEM CLK on 1C0N: the MARIO chip needs a 21.477 MHz oscillator module on the
cart's 5 V supply, with its output on pin 1. This is done outside the Pico; the
firmware has no clock output (GP21/23/24/25, the RP2040's clock output pins,
carry A10-A14). Confirmed on a Japanese Star Fox cart.

**Driven by the Pico (through level shifters)**

| Pico    | Cart signal | Connector pins |
|---------|-------------|----------------|
| GP0-9   | A0-A9       | 17,16,15,14,13,12,11,10,9,8 |
| GP10-17 | D0-D7       | 19,20,21,22, 50,51,52,53 |
| GP18    | /RD         | 23 |
| GP19    | /WR         | 54 |
| GP20    | /RESET      | 26 (bidirectional on the console; the firmware only drives it) |
| GP21-25 | A10-A14     | 7, 6, 37, 38, 39 |
| GP26    | A20, A21, A22 joined (bank $70 when High) | 45, 46, 47 |
| GP27    | /ROMSEL (Low selects the cart's ROM/RAM region) | 49 |

A10-A14 are driven so the same wires select the GSU registers (A12=A13=1,
the rest 0: $3000-$33FF) or the cart RAM window (A13=A14=1, A10-A12 = 1 KB
block: $6000-$7FFF). They sit at the register levels except while the monitor
reads RAM. If you wired them to fixed levels before (A12, A13 high, A10, A11,
A14 low), move those five wires to GP21-GP25.

**Hard-wired levels** (so the cart decodes bank $00)

| Level | Signals (connector pins) |
|-------|--------------------------|
| Low   | A15 (40), A16-A19 (41-44), A23 (48) |

A20-A22 (45-47, joined) and /ROMSEL (49) come from GP26 and GP27 and idle at Low and
High, which is what the earlier hard-wiring did. (Boards wired the old way still work;
only bank $70 mode needs the change.)

A23 matters only on 1CB5B-01 and /ROMSEL only on 1C0N; elsewhere the pins are
not connected on the PCB and the level is ignored.

**Power**

| Signal | Pins | Notes |
|--------|------|-------|
| +5V    | 27, 58 | power the cart from 5 V |
| GND    | 5, 36 | common with the Pico |

**Leave open**

| Signal | Pin | Notes |
|--------|-----|-------|
| Expansion group A: SYSTEM CLK, EXPAND, PA6, /PRD, /WRAMSEL, REFRESH, PA7, /PWR | 1, 2, 3, 4, 32, 33, 34, 35 | unused and unconnected on all three boards, except SYSTEM CLK on 1C0N: leave open. The oscillator is on the cart, so no external clock is needed (on 1C0N, pin 1 takes the external clock module) |
| Expansion group B: PA0, PA2, PA4, left audio, PA1, PA3, PA5, right audio | 28, 29, 30, 31, 59, 60, 61, 62 | unused and unconnected on all three boards: leave open |
| PHI2 | 57 | CPU clock, not connected on any of the three boards: leave open |
| /IRQ | 18 | cart output; the host polls SFR.GO and masks IRQ in CFGR, so leave open (pull-up if you use it later) |
| CIC data 1, CIC reset, CIC data 2, CIC CLK | 24, 25, 55, 56 | the CIC is on every cart but not connected to the Super FX, so it is not needed; leave open |

Cart ROM is never read by the host. Cart RAM is read only by the monitor
(through the window above, with the GSU stopped). The host polls SFR.GO
instead of using /IRQ.

## Startup text, then binary

After reset the firmware prints a line such as

    GSU host v1.4.0: VCR=01 SFR=0000 CLK=10.7MHz MODE=loader

with the version register, the status flags, the clock the host selects
(`CLK` is the value the firmware writes to CLSR; CLSR cannot be read back) and
the build mode (`loader`, `measure` or `diag`).

In the default (`loader`) build the firmware then enters the monitor
(`mon> `) and does not start the GSU. Type `l` (or send `l\r`) to load the
loader; it prints `READY\r\n`. `i` prints the banner line again, so a PC app
that connects after the reset can still read the clock. The measure build has
no monitor at start: it starts the GSU and prints `READY\r\n` right away.

Everything after `READY\r\n` is the raw byte stream of the GSU program's
read/write syscalls. A PC app should read lines until `READY`.

## RAM loader (default)

The loader starts in two stages, the same on every chip. The firmware puts a
15-byte stage-1 program (`boot_prog`, one cache line) in the GSU instruction
cache, presets R1, R2, R4, R8, and starts it. Stage 1 reads the first
`sizeof loader_prog` (80) read syscalls, which the firmware serves from its own
copy of the loader, stores them in cart RAM at $0000-$004F with STB and
LJMPs to $70:0000. The 65-byte loader then runs from RAM (the cache fills from
RAM on misses). It is no longer run from the cache on the GSU-1; one path is
used everywhere. Send, little endian, after `READY`:

| Bytes   | Meaning |
|---------|---------|
| 2       | load address in cart RAM (RAM bank 0 = $70) |
| 2       | entry address (jumped to after loading) |
| 2       | payload length, at least 1 |
| len     | payload |

The loader reads each byte with the read syscall, stores it with STB, then
runs LJMP to bank $70 at the entry address. The loaded program keeps using
the same syscalls through STOP. Notes:

- **Convention: load and enter at $0200.** Use $0050-$01FF as scratch RAM
  for variables: `LMS Rn,(yy)` and `SMS (yy),Rn` reach $0000-$01FE with 2-byte
  instructions (address = yy x 2), so yy = $28 and up. Its contents at start
  are undefined (the previous run's data), so write a variable before reading
  it. **Never write $0000-$004F:** that is where the loader stays, and on the
  MC1 `l` after a program has run restarts the loader from there (see below).
  $0200 is also above the loader, so one binary runs on every chip.
- **Start the program with `CACHE` (`$02`).** The loader jumps with LJMP from
  $004E, which leaves CBR=$0040; without `CACHE` only $0200-$023F of the program is
  cached and everything after it runs at ROM/RAM speed (about 3 cycles per
  byte). `CACHE` makes the window $0200-$03FF (see "Multiplier speed").
- Assemble the payload for the address you load it at. Load and entry
  addresses are independent, so data can sit below the code.
- On the MC1, `l` after a program has run (from the monitor after an exit) does
  not run stage 1 again: the MC1 keeps the cache window and cache lines of the
  program that ran and ignores host writes to them (an experiment showed stage 1
  could not be started: the fetch at $0000 returned 00 = STOP). The firmware starts the
  loader that is still in RAM at $0000 directly, with PBR=$70. This only works
  if the program left $0000-$004F alone; otherwise reset the board. The GSU-1 and
  GSU-2 run stage 1 again every time.
- The loader itself sits in cart RAM at $0000-$004F while it runs, so load
  the program at $0050 or above (by the convention above, $0200). Loading
  over $0000-$004F overwrites the running loader.
- Length is limited by the cart's RAM size (commonly 32 KB or 64 KB), and
  the loader does not check it.
- Speed is set by the syscall round trip. A read syscall costs 5 bus reads and
  4 bus writes (the NOP syscall's 5 + 2, plus the 2 writes that load R0), about
  16 us, so roughly 60 KB/s. This is calculated from the measured bus times;
  the loader's speed has not been timed (see "Optimization ideas").
- The loader assumes RAMBR = 0 (bank $70) after reset and that SCMR gives
  the GSU the RAM (RAN/RON), which the firmware sets.

## System calls

The GSU program sets R3 (low byte) to the op and executes STOP. The host
sees GO cleared, services the call, and resumes by rewriting R15. The numbers
follow Unix (exit 1, read 3, write 4, fstat 28, times 43; i386 numbering) where there is a
counterpart.

| R3 | Name   | R0 low byte                                              |
|----|--------|----------------------------------------------------------|
| 0  | nop    | unchanged (host does nothing; used by the measurement build) |
| 1  | exit   | control returns to the Pico: it prints `EXIT R15=xxxx` and enters the monitor |
| 3  | read   | out: next byte from USB serial (blocks until available)  |
| 4  | write  | in: byte sent to USB serial                              |
| 28 | fstat  | out: bit0 = RDRF (RX byte available), bit1 = TDRE (TX can accept a byte) |
| 43 | times  | reserved, not implemented (`BAD OP 2B`)                   |

An unknown number prints `BAD OP xx R15=xxxx` and enters the monitor (`r` shows
the registers; `g` and `t` are refused, `l` loads a program).

The status bits follow the low two bits of a 6850 ACIA status register.
Bits 7..2 are returned as 0. TDRE is "USB connected and the CDC write FIFO
has room" (set USE_TUD_TX_STATUS to 0 to use "USB connected" only).
The read and fstat results are written to R0 as a zero-extended 16-bit value
(the high byte is cleared); the write syscall uses only the low byte of R0.

Resume rewrites R15 with the value it already holds (low byte, then high
byte; the high-byte write sets GO). This works for code at any address.

## Monitor (exit syscall) and RAM window

The monitor is also the first thing the default build shows after reset (`d`, `r`
and `i` work, `g` does not until a program has run; `l` starts the loader).

A GSU program that runs `IBT R3,#1; STOP` (syscall 1, exit) stops and hands
control to the Pico. The firmware prints `\r\nEXIT R15=xxxx\r\n` (R15 as left by
STOP, i.e. the STOP address + 2), then a `mon> ` prompt. From here the stream
is text again: a PC app has to treat the `EXIT` line as the end of the binary
stream (it cannot be told apart from program data, so for programs that use
exit, send a marker of your own just before it if you need to be sure).
Numbers are hex. `d` prints 16 bytes per row, rows aligned to 16, and leaves
blank the bytes outside the requested range. RAM is 0000-1FFF; an `eaddr` past
the end is cut off there.

| Command | Action |
|---------|--------|
| `d` | dump 256 bytes of cart RAM after the last dump (the first time from 0000) |
| `d saddr` | dump 256 bytes from `saddr` |
| `d saddr,eaddr` | dump `saddr` <= a <= `eaddr` (inclusive); addresses 0000-1FFF through the $6000 window, 0000-7FFF through bank $70 (see `b`) |
| `b` / `b 0` / `b 1` | how `d` reads the RAM: `b` = auto (bank $70 on the MC1, the $6000 window on the others; the default), `b 0` = $6000 window (8 KB), `b 1` = bank $70 (32 KB, needs GP26 and GP27 wired, see below) |
| `r` | R0-R15, SFR, PBR, CBR (PBR and CBR read as `n/a` on the MC1, where they are not readable) |
| `g` | resume the GSU after the exit, as if it were any other syscall (not possible before the first `l`) |
| `t` | like `g`, and at the next exit also print the elapsed time: `time 12.345 ms (GSU run 12.100 ms, 34 syscalls)`: wall time from the resume to the exit (it includes waiting for USB in read and write syscalls), the time the GSU itself ran, and the number of syscalls. To time only the program, let it exit once at its start, load, then `t` to the final exit (the loading time is not counted). `l` does not start a timer |
| `l` | load: boot the loader (prints `READY`, then send a program); also reloads after an exit |
| `c 0` / `c 1` | GSU clock 10.7 MHz (default) / 21.4 MHz; applied at the next `l`, or at `g` after an exit if it changed (CFGR/CLSR are written only when the value changed); refused while `m 1` is set |
| `m 0` / `m 1` | multiplier standard (default) / high speed (CFGR.MS0); applied at the next `l` or `g` (same rule); refused while the clock is 21.4 MHz |
| `i` | print the banner line again (VCR, SFR, current CLK, MODE, MS0) |

The settings are not saved: after a reset the defaults (10.7 MHz, standard
multiplier) are back, so type `c` and `m` again before `l` if you want something
else. A warning is printed for `c 1` on the MC1, which runs away at 21.4 MHz in
longer programs.
| `?` | help |

The window works on the GSU-1 and GSU-2; on the MC1 it shows no RAM.

**Bank $70 mode** (`b 1`, the default on the MC1) reads the RAM the way the S-CPU
does in bank $70: A20, A21 and A22 High (all three cart pins 45-47, joined, driven by
GP26), /ROMSEL Low (cart pin 49, driven by GP27), A15-A19 and A23 Low, and A0-A14
from GP0-GP9 and GP21-GP25, so offsets 0000-7FFF (32 KB; a smaller RAM is mirrored)
are reachable. GP26 idles Low and GP27 High (the levels the earlier hard-wiring
had), and `d` switches them only while it reads, with SCMR.RAN=0 as for the window.
On a board without these two wires nothing changes except in bank mode, where the
dump does not show the RAM. Confirmed on the MC1: with the two wires fitted, `d` shows
the RAM in bank $70 (after `l` the loader's bytes `A3 03 ...` are at $0000), so the MC1
maps its RAM there with /ROMSEL low. Confirmed on the GSU-1: `b 0` and `b 1` give
identical dumps. Beyond the RAM's size the dump shows the RAM mirrored (confirmed on the MC1).
`[VERIFY]` the GSU-2 (not run with this mode).

Cart RAM is read through the cart's S-CPU window at $6000-$7FFF in bank $00
(A13=A14=1, A15=0), which shows the first 8 KB of the RAM the GSU sees at
$70:0000. The Pico drives A10-A14 on GP21-GP25 for this, and sets SCMR.RAN=0
(RAM to the S-CPU) only while it reads, so the GSU must be stopped. Only the
first 8 KB are reachable (A15 and above are fixed). Quick check after loading
a program: call exit, then `d 0,4F` should show the loader's bytes
(`A3 03 00 01 11 B0 9E A3 ...`) at the start of RAM. A minimal program for this
is `IBT R3,#1; STOP; NOP; BRA` back (7 bytes at $0200, so every `g` returns to the
monitor): type `l`, wait for `READY`, send `00 02 00 02 07 00 A3 01 00 01 05 FA 01`, then use `d`.

## Host register rules (from fullsnes, GSU-n chapter)

- **R0-R15 writes are latched.** A write to the even address (LSB) only sets
  LATCH; the write to the odd address (MSB) applies LSB=LATCH and MSB=data. A
  register changes only on its MSB write, so always write LSB first, then MSB.
  The firmware does this in `gsu_reg16_write()`.
- **A write to R15's MSB (301Fh) sets GO** and starts the GSU.
- **While GO=1, only SFR, SCMR and VCR may be accessed.** SFR can be read and
  written while running. The firmware only polls SFR while the GSU runs (and
  the monitor only reads RAM while it is stopped).
- **Loading the cache:** write SFR=0000h (GO=0) first; that forces CBR=0 and
  marks every line empty. Then write the code in 16-byte lines; writing a
  line's last byte marks it valid. CBR is read-only for the host.

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
  hang right after a RAM write there (confirmed, see "STB-then-STOP hang
  test"; two NOPs are enough). The loader keeps several instructions between STB and the next STOP.
  GSU-1 (tested) and GSU-2 are not affected.

## Echo demo

A 21-byte program for the loader (load and entry `$0200`): it waits until
RDRF is set (fstat syscall), reads a byte, adds 1, and writes it back, so
each character you type returns as character + 1. Type `l`, wait for `READY`,
then send the stream `00 02 00 02 15 00` + the bytes below. (Confirmed earlier as a cache-resident build: typing `123`
returns `234`.) Each byte costs three syscalls (fstat, read, write), about
37 us, so the echo loop tops out near 27 KB/s. Bytes, `$0200`:
`A3 1C 00 01 3E 71 09 F8 01 A3 03 00 01 D0 A3 04 00 01 05 EC 01`.

## Diagnostics (`-DGSU_DIAG=1`)

Build with `cmake -DCMAKE_C_FLAGS=-DGSU_DIAG=1 ..`. It takes precedence over
the other modes. After reset it prints PASS/FAIL lines over USB serial, then
waits for a key to run again:

1. VCR, SFR, CBR readout.
2. SFR flag bits (1-4) write/readback, then R0-R13 write/readback patterns, one unique value per register (address
   lines), and the latched-write rule.
3. Cache write/readback, $3100-$32FF (a mismatch alone does not prove a fault
   if the cache is not host-readable; test 4 decides).
4. Start test: `IBT R1,#5A; STOP; IBT R2,#77; STOP`, run once with the normal
   CFGR/CLSR and once with both 0, checking GO, R1, R2, R15 and the resume.
   These short tests pass at any clock speed.
5. Clock check: times a 65535-iteration cache loop with CLSR=00 and CLSR=01
   and prints the ratio (about 2.0 if the 21 MHz mode works, about 1.0 if
   the chip ignores CLSR). CLSR is write-only, so timing is the only check.
6. Cache line tests: small programs run from line 1 only, from line 31, across
   the line 0/1 boundary, and with a STOP at the start of a line followed by a
   resume mid-line. Added after the MC1 echo demo found line 1 overwritten with
   ROM code.
7. Last-block test (the MC1 behaviour, see its section).
8. Cart RAM window ($6000-$7FFF, A10-A14). 8.1 (informational) reads four
   addresses in each 1 KB block with the data lines pre-driven to 00 and to FF;
   a `*` means the value follows the pre-driven level (floating bus). A stable
   00/00 does not prove the cart answers; a pulled-low bus looks the same.
   8.2 a GSU program stores a byte into RAM and the window must show it.
   8.3 writes a different byte into each block through the window and reads it
   back (aliased blocks point at a stuck or swapped A10-A12 wire). 8.4 the host
   writes a byte through the window and the GSU loads it (LDB) into R0: shows
   whether window writes reach RAM even if window reads do not. 8.5 reads the
   byte 8.2 stored with SCMR = 00, 08, 10 and 18, in case the S-CPU needs another
   ownership setting. Finally checks that the registers still work. All pass on
   the GSU-1 and GSU-2 (8.1-8.3 of the first version); the MC1 (board 1C0N)
   fails 8.2-8.4 and 8.5 shows only 00 (see "Verification status").

Reading the result: step 2 failing points at the bus (level shifter direction,
data lines, /WR timing); step 2 passing but step 4 failing points at GO not
starting, the cart clock or the A10-A14 levels; step 4 passing only with
CFGR/CLSR = 0 points at the clock configuration.

## Measuring throughput (`-DGSU_MEASURE=1`)

Build with the macro, for example `cmake -DCMAKE_C_FLAGS=-DGSU_MEASURE=1 ..`.
It takes precedence over the loader. Open the USB serial port;
the firmware prints two lines and then halts:

    MEASURE bus: read=... ns, write=... ns (sys clock ... Hz, BUS_DELAY_CYCLES=...)
    MEASURE syscalls: n=1000000 total=... us avg=... ns rate=... /s polls_x100=...

- `bus` times 10000 raw register reads (SFR) and writes (R13, unused) before
  the GSU starts. Compare with your expectation (about 1.5 us per access at
  125 MHz with `BUS_DELAY_CYCLES` 64).
- `syscalls` times `MEASURE_COUNT` (default 1000000, override with
  `-DMEASURE_COUNT=...`) NOP syscalls issued by the GSU in a loop, with no USB
  data traffic, so it is the bus + polling cost per syscall. Syscalls per second
  is also the bytes per second the loader can reach.
- `polls_x100` is the average number of SFR polls per syscall, times 100.
  100 means the GSU had already stopped at the first poll; larger means the
  host waited for it.
- Measured on the GSU-1 cart (125 MHz, `BUS_DELAY_CYCLES` 64, 21 MHz GSU; not
  measured on the MC1):

      MEASURE bus: read=1686 ns, write=1712 ns
      MEASURE syscalls: n=1000000 total=12368118 us avg=12368 ns rate=80853 /s polls_x100=100

  A syscall is 7 bus accesses: 3 reads to see the stop and get the op (SFR low
  poll, SFR high, R3), then 2 reads and 2 writes to resume (R15 get and set).
  That is 5 x 1686 + 2 x 1712 = 11.85 us; the other ~0.5 us is loop overhead.
  The GSU's own execution time is negligible, so the rate is set by the host's
  bus speed. `polls_x100=100` means one SFR poll per syscall: the GSU had
  already stopped by the first poll. A write syscall adds one bus read (R0),
  about 14 us, roughly 71 KB/s; a read syscall adds the two writes to R0,
  about 16 us, roughly 63 KB/s (calculated, not measured).
- To go faster, lower `BUS_DELAY_CYCLES` (about 200 of the ~210 cycles per
  access are the three delays) and re-run the diagnostic build to find the
  smallest value that still passes. Not tried yet (see "Optimization ideas").
- The run is silent between the two lines on purpose: printing during the
  loop would pollute the timing.
- Cross-check with a logic analyzer on /RD and /WR if you have one.

## STB-then-STOP hang test

The test program (load address and entry `$0200`, writes to `$0400`; works on
both chips, since `$0200` is above the stage-1 area) runs six phases. Each prints
an uppercase letter, runs 10000 "RAM write, then STOP" iterations (the STOP is
the nop syscall), and prints the lowercase letter (type `c 0` on the MC1, the default):

| phase | code before STOP |
|-------|------------------|
| A | `STB (R5)` directly followed by STOP |
| B | `STW (R5)` directly followed by STOP |
| C | `STB (R5)`, one NOP, STOP |
| G | `STB (R5)`, two NOPs, STOP |
| H | `STB (R5)`, three NOPs, STOP |
| D | `STB (R5)`, four NOPs, STOP |

Expected output when nothing hangs: `AaBbCcGgHhDdEND`, then the GSU waits for a key.
If a phase hangs, its lowercase letter is missing and the output stops (a build
with `-DGSU_WATCHDOG_MS=500` also prints `TIMEOUT: GSU still running, SFR=0020`
lines); reset the Pico to recover. The first
phase that does not finish shows which pattern is affected (A/B alone means
"immediately after the write"; C too means one instruction is not enough).

Result: on the GSU-1 (VCR=04, 21.4 MHz) phases A-D finish, so STB-then-STOP
does not hang there (G and H were added later and not run on it). On the MC1
(VCR=01, 10.7 MHz) A, B and C hang (`TIMEOUT ... SFR=0022`, GO still set, the
lowercase letter is missing), while G, H and D finish (G and H run twice each).
So on the MC1 both STB and STW hang when STOP follows directly or after one
NOP; two or more instructions in between are enough. The loader and stage 1
keep five instructions between STB and STOP, so they are safe.

## Multiplier speed (CFGR.MS0)

CFGR bit 5 (MS0) is a high-speed multiply mode that must stay 0 at 21 MHz.
The monitor's `m 1` sets it (it is refused while the clock is 21.4 MHz, and `c 1`
is refused while MS0 is set); `i` then shows ` MS0=1` in the banner. The
diagnostic build always uses CFGR=80.

Method: one small program per instruction (load and entry `$0200`): `CACHE`,
then a `LOOP` over 32 identical operations (56470 iterations) from the cache, then
EXIT and a jump back to the start. `l`, send the file, then `t` prints the time of
a full run (`GSU run`). Variants: `N` (32 NOPs, baseline), `M` MULT, `U` UMULT
(ALT1 + MULT), `F` FMULT, `L` LMULT (ALT1 + FMULT). Cycles per operation =
(T_X - T_N) / (56470 x 32) x clock + 1; the `U` and `L` rows include the prefix
byte. The clock comes from the NOP baseline (1 cycle each): 10.708 MHz and
21.415 MHz on the GSU-1A cart.

**Start every program with `CACHE` ($02).** The loader's final LJMP leaves
CBR=$0040 (the address it jumps from, not the target), so the cache window is
$0040-$023F and a program at $0200 gets only its first 64 bytes cached. `CACHE`
as the first instruction sets CBR = R15 & $FFF0 = $0200 and the window becomes
$0200-$03FF (512 bytes). Measured with N NOPs in a LOOP, GSU-1A and MC1: without
`CACHE` the cost per instruction is 1.00 / 1.55 / 2.26 / 2.62 cycles for N = 32 / 64 /
128 / 256 (about 48 loop bytes cached, the rest at about 3 cycles per byte); with
`CACHE` it is 1.00 for all of them.

Results (GSU-1A, 1CA6B-01, VCR=04, from the cache):

| Clock | MS0 | MULT | UMULT (ALT1+) | FMULT | LMULT (ALT1+) |
|-------|-----|------|---------------|-------|---------------|
| 10.7 MHz | 0 | 1 | 3 | 8 | 9 |
| 10.7 MHz | 1 | 1 | 2 | 4 | 5 |
| 21.4 MHz | 0 | 1 | 3 | 8 | 9 |

The same in time:

| Clock, MS0 | MULT | UMULT | FMULT | LMULT |
|------------|------|-------|-------|-------|
| 10.7 MHz, 0 | 93 ns | 280 ns | 747 ns | 840 ns |
| 10.7 MHz, 1 | 93 ns | 187 ns | 374 ns | 467 ns |
| 21.4 MHz, 0 | 47 ns | 140 ns | 374 ns | 420 ns |

Documented cycle counts (instruction pages on sneslab.net, cache speed):
MULT 1 or 2, UMULT 2 or 3, FMULT 4 or 8, LMULT 5 or 9 (ROM/RAM speed adds 2-3 or
more). UMULT, FMULT and LMULT match them: MS0=0 gives the larger value, MS0=1 the
smaller one. MULT matches too, with one detail: in a run of identical MULTs it
costs 1.03 cycles at MS0=0 (1.00 at MS0=1), but an isolated MULT (MULT ; NOP, 16
times) costs 2 cycles at MS0=0 and 1 at MS0=1, at 10.7 and at 21.4 MHz. So the
second cycle of a standard-speed MULT overlaps with a MULT that follows it, and
the table's MULT 1 is the cost inside a run. The UMULT/FMULT/LMULT rows were also
measured in runs; isolated costs were not measured. `[VERIFY]`

Findings.

1. **MS0=1 works on the GSU-1A at 10.7 MHz:** UMULT, FMULT and LMULT take 1, 4
   and 4 cycles less (FMULT 747 ns -> 374 ns); MULT (8 x 8) does not change.
2. **The multiplier scales with the clock from the cache.** At 21.4 MHz with
   MS0=0 the cycle counts are the same as at 10.7 MHz, so every multiply takes
   half the time; FMULT (374 ns) equals 10.7 MHz with MS0=1, and the others are
   faster. MS0 must stay 0 at 21.4 MHz, so on the GSU-1/GSU-2 use `c 1` for
   everything; `m 1` only helps when you stay at 10.7 MHz.
3. An earlier benchmark started without `CACHE`, so its multiply loops ran
   outside the cache window (ROM/RAM speed): GSU-1A 10.7 MHz MS0=0 gave 3.35 / 9.27 /
   11.28 / 14.29 cycles and MS0=1 3.25 / 6.25 / 7.24 / 10.24, 21.4 MHz 5.54 / 11.53 /
   12.53 / 17.55 (so "multiplies gain only 8-15 % at 21.4 MHz" was a result of
   that, not of the multiplier). On the MC1 the same benchmark gave 3.33 / 9.22 /
   11.21 / 14.20 with MS0 0 and 1 alike, so the MC1 seems to ignore MS0. `[VERIFY]`
   with the cached programs, which were not run for the multiplies on the MC1.
4. **MC1, from the cache** (10.7 MHz, runs of identical instructions): MULT 1.03,
   UMULT 3.00, LMULT 9.00 cycles, the same with MS0 0 and 1, so the MC1 ignores
   MS0 (confirmed with the cached programs). FMULT is 8.00 cycles with a nonzero
   operand (`FROM R1 ; FMULT`) but 4.78 in a run where R0, its source, has become
   0: on the MC1 the FMULT time depends on the data. (The GSU-1A took 8 in the
   zero-operand run too; its nonzero case was not measured.) `[VERIFY]` At 21.4 MHz
   the MC1 does not return from the test (the known runaway).
5. **MC1 hangs** on two loop bodies that run on the GSU-1A (isolated MULT test
   and FROM + LMULT, both measured there): 16 x (MULT ; NOP), and 32 x
   (FROM R1 ; ALT1 ; LMULT). It does not matter whether the loop is closed
   with LOOP or with DEC R12 ; BNE, whether the program starts with `CACHE` or
   `CACHE ; NOP`, or whether it runs 1000 or 56470 iterations: all of these
   hang. Runs of identical multiplies (`N M U F L`), 32 x (FROM R1 ; FMULT) and
   `mult_check` (each multiply once, straight line) are fine. The cause is
   unknown; a multiply followed by a different instruction (NOP, FROM) is the
   common point of the hanging bodies, but FROM after FMULT does not hang.
   `[VERIFY]` Avoid loops on the MC1 that mix multiplies with other
   instructions until this is understood; the GSU-1A is not affected. A small
   loop of NOPs written as `CACHE ; NOP ; FOR: ... ; BRA FOR ; NOP` runs from the
   cache on the MC1; without the NOP after `CACHE` it hung after the first pass
   (reported, not reproduced with a test file).
6. GSU-2: not measured, assumed like the GSU-1A (same VCR=04 family).

All cached results agree with the documented cycle counts (cache speed); the
documentation says nothing about data dependence, the MC1's ignoring of MS0, or
the hangs. The earlier uncached figures cannot be compared with the ROM/RAM
column: only the first 64 bytes of a program were cached, so the numbers are a
mixture.

Use: on the GSU-1/GSU-2 start programs with `CACHE` and use `c 1` (21.4 MHz);
at 10.7 MHz (`c 0`) add `m 1` for multiply-heavy code. On the MC1 stay at `c 0`.

## Verification status

Confirmed on the cart:

- Register window decode with A10-A22 at fixed levels (since changed: A10-A14
  are now driven from GP21-GP25, see below), bus reads and writes,
  BUS_DELAY_CYCLES = 64, SFR flag bits, R0-R13, the latched-write rule.
- Cache write/readback, GO set by the R15 high byte, running from the cache
  (CBR=0), STOP leaving R15 = STOP + 2, resume by rewriting R15, at 21 MHz and
  at 10.7 MHz.
- Syscalls read, write and status (echo demo, before the status call became fstat), USB serial round trip.
- The RAM loader: payload received, stored with STB, LJMP to bank $70 and
  execution from cart RAM, with the loaded program using the syscalls, through
  the two-stage boot on the GSU-1 (VCR=04) and the MC1 (10.7 MHz).
- On the MC1, `l` after a program has run restarts the RAM loader directly
  (PBR=$70, no stage 1): reload, run, `g` and `l` again work. Stage 1 cannot be
  run again there (the MC1 keeps the cache window of the program that ran).
- The monitor at start-up (`mon> `): `l` loads the loader (`READY`), `i` prints
  the banner, `c 0|1` and `m 0|1` set the clock and MS0 with the refusals between
  `c 1` and `m 1` (on the GSU-1 and the MC1).
- The exit syscall, `r`, `g`, `t` (elapsed time at the next exit; an echo program
  waiting for a key: 1022 ms wall, 176 ms GSU run, 48903 fstat polls, about
  21 us per syscall) and `l` after an exit, on the MC1 and the GSU-1; `d` through the $6000 window on the
  GSU-1; `c` / `m` with `t` on the GSU-1 (the program keeps working).
- The `t` figure "GSU run" has a floor of about 1.9 us per syscall (the Pico
  polls SFR over the slow bus), the same at 10.7 and 21.4 MHz. It is accurate
  for long computations between syscalls, not for syscall-heavy loops.
- The cache window: after the loader, CBR=$0040 (window $0040-$023F); a program that starts with `CACHE` runs 256 NOPs in a loop at 1 cycle each (GSU-1A and MC1).
- Throughput of the syscall path (measurement build).
- The Pico SDK build, including `tusb.h` for `USE_TUD_TX_STATUS`.
- The GSU clock source is the oscillator on the cart; the chip is a GSU-1
  (VCR=04).

- The hand-assembled echo demo (+1) was assembled with the bass
  assembler by the user: the generated bytes match, and it runs on both the
  GSU-1 and the MC1. (`loader_prog` and `boot_prog` were not compared with
  assembler output; they run correctly on the cart.)
- MC1 results (diagnostics, two-stage boot, 10.7 MHz) and the GSU-1 / MC1
  comparison are in the MC1 section above and in
  `docs/cache-and-chip-notes.ja.md`.

Confirmed by the hang test: an STB or STW followed by STOP with at most one
instruction (NOP) in between hangs the MC1 but not the GSU-1; two are enough.

Still to verify:

1. That `c` / `m` really change the clock and MS0 after an exit (`t` or `g`):
   the commands run and the program keeps working, but the effect is not
   measured (the "GSU run" figure of `t` is dominated by polling latency for
   syscall-heavy programs).

Not tested, assumed like the GSU-1 (VCR=04 family): the monitor and CFGR.MS0 on the
GSU-2. A10-A14 and the RAM window are confirmed on the GSU-2 (diag).

Confirmed with the A10-A14 pins: all diagnostic checks pass on the GSU-1 and the
GSU-2, and [1]-[5] pass on the MC1.

The RAM window: confirmed on the GSU-1 and the GSU-2 (the first version of
diag [8], host write through the window and read back, passes on both), and
found not to work on the MC1 (board 1C0N, 10.7 MHz). Full diag [8] on the MC1:
reads give a constant 00 whatever the data lines were pre-driven to (a bus
pulled low, or the cart driving 00), or the stale 10 (the SCMR value written
just before); a byte the GSU stored (5A) is never seen, with SCMR = 00, 08, 10
or 18 (8.5); and bytes the host writes through the window never reach RAM
(8.4: the GSU's LDB at $0020 returned the same 1C after writes of 3C and C3).
The registers stay reachable. So with /ROMSEL held high the S-CPU window
$6000-$7FFF shows no RAM on the MC1, in either direction. Most likely the MC1
maps its RAM only at $70-$71 (where the S-CPU reaches it with /ROMSEL low); not
tested. Since 1.4.0 the monitor's `d` reads the MC1's RAM through bank $70 instead:
A20-A22 are driven together by GP26 and /ROMSEL by GP27 (see "RAM dump"); confirmed on
the MC1. An alternative that needs no wiring, a GSU program that reads the RAM and sends
it with the write syscall, is not built.
(Loader speed and `BUS_DELAY_CYCLES` tuning moved to "Optimization ideas".)

Open questions, cause unknown (no action planned; notes and ideas for
investigating each are in `docs/cache-and-chip-notes.ja.md`, section 5):

- The MC1 runs away at 21 MHz in long programs (short diagnostics pass).
- Why the MC1 uses host-written cache code only in the block that ran last.
- Why SFR reads 8000 after STOP on the MC1 (0000 on the GSU-1); harmless, the
  firmware clears it.

## Optimization ideas (not done)

The syscall rate is set by the host's bus accesses (7 per syscall, about
1.7 us each; see "Measuring throughput"). Figures below are calculated from the
measured bus times, not measured.

1. **Lower `BUS_DELAY_CYCLES`** (default 64). About 190 of the ~210 cycles per
   access are the three delays. At 32, each access drops by about 0.8 us and a
   syscall from about 12.4 us to about 7 us (about 140,000 syscalls/s). Find
   the smallest value that passes the diagnostic build (`-DGSU_DIAG=1`) on
   each cart, with margin; the GSU-1 and the MC1 may differ. Then time the
   loader with the new value (loader speed has only been calculated, about
   60 KB/s at 64).
2. **Skip the SFR high read** after STOP. It only clears the IRQ flag, which
   nothing uses (/IRQ is open, completion is polled). Saves 1 of 7 accesses.
   Untested on the MC1, where the flag stays set after STOP.
3. **Block syscalls**: let the GSU pass an address and a count in registers so
   one STOP moves many bytes (the 12 us cost is paid once per block, not per
   byte). The biggest gain for bulk transfers; needs new syscall numbers and
   GSU-side loops.
4. **Resume with fewer accesses**: the current resume reads R15 (2) and
   rewrites it (2). A single high-byte write would save 3 accesses, but the
   low byte is latched, so it only works if the low byte is rewritten too; the
   current method was chosen deliberately as the most general. Do not change
   it without asking.
5. **PIO bus**: move the strobes and data transfer to the RP2040 PIO to cut the
   per-access time below what the delay loops allow.

## Sources

- SNES cartridge connector pinout (SNESdev Wiki): https://snes.nesdev.org/wiki/Cartridge_connector
- Fullsnes (Nocash SNES Specs), SNES Cart GSU-n chapters: https://problemkaputt.de/fullsnes.htm
- Super FX instruction set table (Yoshi's Island disassembly wiki): https://github.com/brunovalads/yoshisisland-disassembly/wiki/Super-FX-Instruction-Set
- rmstdope/neser PR 3205 (cache clearing conditions): https://github.com/rmstdope/neser/pull/3205
- sp00nznet/LakeSnes PR 2 (byte after a jump runs first): https://github.com/sp00nznet/LakeSnes/pull/2

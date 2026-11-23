# gsu-host

Pico (RP2040) firmware that drives a real Super FX cartridge as a
stand-alone processor.

Status: confirmed on a real GSU-1 cart (board 1CA6B-01) and on a MARIO chip
(MC1) cart (board 1C0N). The MC1 differs; see the MC1 section.

- Diagnostic build (`-DGSU_DIAG=1`): bus reads and writes, SFR flag bits,
  R0-R13 and the latched-write rule, cache write/readback, start (GO via the
  R15 high byte), STOP with R15 = STOP + 2, and resume by rewriting R15, at
  CFGR/CLSR = 80/01 (21 MHz) and 00/00. All checks pass.
- Echo demo (`-DGSU_PROGRAM_DEMO=1`): each typed character returns +1, so the
  status, read and write syscalls work end to end over USB serial.
- Measurement build (`-DGSU_MEASURE=1`): see "Measuring throughput" for the
  numbers (about 12.4 us and 80,900 syscalls/s).
- RAM loader: the user loaded a binary build of the echo program through it
  and it ran (`Hello,world` came back as `Ifmmp-xpsme`). This confirms the
  loader's hand-assembled code, STB to cart RAM, LJMP to bank $70 and the
  loaded program's syscalls. On the MC1 this works through the two-stage boot
  (`-DGSU_MC1=1`); the cache-resident echo demo (32 bytes) does not work on
  the MC1.
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
    # 10.7 MHz GSU clock (for the MC1): cmake -DCMAKE_C_FLAGS=-DGSU_CLOCK_21MHZ=0 ..
    # MARIO chip (two-stage boot + 10.7 MHz, see the MC1 section): cmake -DCMAKE_C_FLAGS=-DGSU_MC1=1 ..
    # demo program instead of the loader: cmake -DCMAKE_C_FLAGS=-DGSU_PROGRAM_DEMO=1 ..

## MARIO chip (MC1, VCR=01)

Tested on a Japanese Star Fox cart (MC1) with SYSTEM CLK and /ROMSEL supplied
externally. Results of the diagnostic build on this cart:

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

**Use 10.7 MHz on the MC1.** At 21 MHz the echo loader build ran away (no
STOP ever reached, `TIMEOUT: GSU still running, SFR=0020`), although the short
diagnostic tests and the clock check pass at 21 MHz. The cause is not known; the
MC1's rated clock is 10.7 MHz.

Build for the MC1 with `-DGSU_MC1=1`, which selects the two-stage boot and the
10.7 MHz clock. The boot part alone is `-DGSU_BOOT_STAGE1=1` (loader build only): the cache holds a
15-byte stage-1 program (`boot_prog`, fits line 0). The firmware presets R2, R3,
R4, R8 and serves the first `sizeof loader_prog` read syscalls from the
firmware's own copy of the normal loader; stage 1 stores them in cart RAM with
STB and LJMPs to $70:0000, so the normal loader then runs from RAM, which is
the usual GSU way (the cache fills from RAM on misses). The PC protocol is
unchanged. Confirmed on the MC1: with this build, loading the echo binary
works (`Hello,world` came back as `Ifmmp-xpsme`), so the loader, LJMP to
cart RAM, and execution from RAM with the normal cache fill all work on the
MC1 at 10.7 MHz.

## Wiring (5 V cart <-> 3.3 V Pico: level shifters required)

Pin numbers are the 62-pin SNES cartridge connector, from the SNESdev wiki
(link in Sources). Directions there are read from a diagram, so treat them as
approximate. Every connector pin is accounted for below.

**Findings from the user's inspection of the cart PCB** (board marking
1CA6B-01 with a GSU-1, as reported by the user; other boards may differ). These override
the wiki-based guesses in the tables below:

- The two 8-pin groups at the cart's two ends (the "expansion terminals",
  8 pins x 2) are all unused and unconnected (confirmed by the user). They
  are pins 1, 2, 3, 4, 32, 33, 34, 35 and pins 28, 29, 30, 31, 59, 60, 61, 62:
  SYSTEM CLK, EXPAND, PA0-PA7, /PRD, /PWR, /WRAMSEL, REFRESH and the
  left/right audio pins. Leave all 16 open.
- The clock oscillator is on the cart.
- PHI2 (CPU clock, pin 57) is not connected.
- A23 (pin 48) and /CART (/ROMSEL, pin 49) are not connected.
- The CIC is not connected to the Super FX, so it plays no part here.

Consequently the GSU clock comes from the oscillator on the cart, not from
the connector. None of these pins needs driving, and the cart runs from power
plus the signals in the first two tables.

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
 6 A11        in  L     | 37 A12        in  H
 7 A10        in  L     | 38 A13        in  H
 8 A9         in  GP9   | 39 A14        in  L
 9 A8         in  GP8   | 40 A15        in  L
10 A7         in  GP7   | 41 A16        in  L
11 A6         in  GP6   | 42 A17        in  L
12 A5         in  GP5   | 43 A18        in  L
13 A4         in  GP4   | 44 A19        in  L
14 A3         in  GP3   | 45 A20        in  L
15 A2         in  GP2   | 46 A21        in  L
16 A1         in  GP1   | 47 A22        in  L
17 A0         in  GP0   | 48 A23        in  open
18 /IRQ       out open  | 49 /ROMSEL    in  open
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
| open | leave open (not needed or not connected on the inspected cart) |
| open* | one of the 16 expansion-terminal pins: confirmed unused and unconnected, leave open |
| +5V, GND | power |

Note for other boards: on the MARIO chip (MC1) board 1C0N, SYSTEM CLK (pin 1)
and /ROMSEL (pin 49) are connected to the chip, unlike on 1CA6B-01. That board
needs an external clock on SYSTEM CLK (a 21.477 MHz oscillator module on the
cart's 5 V supply) and /ROMSEL tied high (to +5 V). Both are done outside the
Pico, so the firmware does not change and `ENABLE_GSU_CLK_OUT` stays 0. (Confirmed
on a Japanese Star Fox cart.)

**Driven by the Pico (through level shifters)**

| Pico    | Cart signal | Connector pins |
|---------|-------------|----------------|
| GP0-9   | A0-A9       | 17,16,15,14,13,12,11,10,9,8 |
| GP10-17 | D0-D7       | 19,20,21,22, 50,51,52,53 |
| GP18    | /RD         | 23 |
| GP19    | /WR         | 54 |
| GP20    | /RESET      | 26 (bidirectional on the console; the firmware only drives it) |

**Hard-wired levels** (so the cart decodes $3000-$32FF in bank $00)

| Level | Signals (connector pins) |
|-------|--------------------------|
| High  | A12 (37), A13 (38) |
| Low   | A10 (7), A11 (6), A14 (39), A15 (40), A16-A22 (41-47) |
| Leave open | A23 (48), /ROMSEL, also called /CART (49): not connected on the inspected PCB |

**Power**

| Signal | Pins | Notes |
|--------|------|-------|
| +5V    | 27, 58 | power the cart from 5 V |
| GND    | 5, 36 | common with the Pico |

**Leave open**

| Signal | Pin | Notes |
|--------|-----|-------|
| Expansion group A: SYSTEM CLK, EXPAND, PA6, /PRD, /WRAMSEL, REFRESH, PA7, /PWR | 1, 2, 3, 4, 32, 33, 34, 35 | unused and unconnected (confirmed): leave open. The oscillator is on the cart, so no external clock is needed; GP21 / `ENABLE_GSU_CLK_OUT` stays off (kept only for carts that take the clock from the connector) |
| Expansion group B: PA0, PA2, PA4, left audio, PA1, PA3, PA5, right audio | 28, 29, 30, 31, 59, 60, 61, 62 | unused and unconnected (confirmed): leave open |
| PHI2 | 57 | CPU clock, not connected on the inspected PCB: leave open |
| /IRQ | 18 | cart output; the host polls SFR.GO and masks IRQ in CFGR, so leave open (pull-up if you use it later) |
| CIC data 1, CIC reset, CIC data 2, CIC CLK | 24, 25, 55, 56 | the CIC is not connected to the Super FX (PCB inspection), so it is not needed; leave open |

Cart ROM/RAM buses are never touched by the host. The host polls SFR.GO
instead of using /IRQ.

## Startup text, then binary

After reset the firmware prints a line such as

    GSU host: VCR=01 SFR=0000 CLK=10.7MHz MODE=boot+loader

with the version register, the status flags, the clock the host selects
(`CLK` is the value the firmware writes to CLSR; CLSR cannot be read back) and
the build mode (`loader`, `boot+loader`, `demo`, `measure` or `diag`), then
`READY\r\n`.
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

- **Convention: load and enter at $0200.** Keep $0000-$01FF free as scratch
  RAM for variables: `LMS Rn,(yy)` and `SMS (yy),Rn` reach exactly that
  range with 2-byte instructions (address = yy x 2). Its contents at start
  are undefined (the previous run's data, or the loader's leftovers at
  $0000-$004F on the MC1), so write a variable before reading it. $0200 is
  also above the MC1 loader, so one binary runs on both chips.
- Assemble the payload for the address you load it at. Load and entry
  addresses are independent, so data can sit below the code.
- With `-DGSU_BOOT_STAGE1=1` (MC1) the loader itself sits in cart RAM at
  $0000-$004F while it runs, so load the program at $0050 or above (by
  the convention above, $0200). Loading over $0000-$004F overwrites the running loader. With
  the default build the loader runs from the cache, so any address works.
- Length is limited by the cart's RAM size (commonly 32 KB or 64 KB), and
  the loader does not check it.
- Speed is set by the syscall round trip. A read syscall costs 5 bus reads and
  4 bus writes (the NOP syscall's 5 + 2, plus the 2 writes that load R0), about
  16 us, so roughly 60 KB/s. This is calculated from the measured bus times;
  the loader's speed has not been timed (see "Optimization ideas").
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
| 3  | nop    | unchanged (host does nothing; used by the measurement build) |

The status bits follow the low two bits of a 6850 ACIA status register.
Bits 7..2 are returned as 0. TDRE is "USB connected and the CDC write FIFO
has room" (set USE_TUD_TX_STATUS to 0 to use "USB connected" only).
The read and status results are written to R0 as a zero-extended 16-bit value
(the high byte is cleared); the write syscall uses only the low byte of R0.

Resume rewrites R15 with the value it already holds (low byte, then high
byte; the high-byte write sets GO). This works for code at any address.

## Host register rules (from fullsnes, GSU-n chapter)

- **R0-R15 writes are latched.** A write to the even address (LSB) only sets
  LATCH; the write to the odd address (MSB) applies LSB=LATCH and MSB=data. A
  register changes only on its MSB write, so always write LSB first, then MSB.
  The firmware does this in `gsu_reg16_write()`.
- **A write to R15's MSB (301Fh) sets GO** and starts the GSU.
- **While GO=1, only SFR, SCMR and VCR may be accessed.** SFR can be read and
  written while running. The firmware only polls SFR while the GSU runs.
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

## Demo program

With `-DGSU_PROGRAM_DEMO=1`, the GSU program waits until RDRF is set
(status syscall), reads a byte, adds 1, and writes it back, so each
character you type returns as character + 1. Confirmed on the cart: typing
`123` returns `234`. Each byte costs three syscalls (status, read, write),
about 37 us, so the echo loop tops out near 27 KB/s.

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

Reading the result: step 2 failing points at the bus (level shifter direction,
data lines, /WR timing); step 2 passing but step 4 failing points at GO not
starting, the cart clock or the fixed address levels; step 4 passing only with
CFGR/CLSR = 0 points at the clock configuration.

## Trace (`-DGSU_TRACE=1`)

Use with `-DGSU_PROGRAM_DEMO=1`. At each STOP the host prints
`[op=.. R0=.. R15=.. CBR=.... SFR=....]` (R15 is STOP address + 2), and
dumps the first 64 cache bytes once, after the 4th line, for the first 40 lines
(`-DTRACE_LINES=n`); repeated status polls are collapsed. Expected sequence for the
echo demo after typing `H`: `[op=02 R0=.. R15=0004]`, `[op=00 R0=0001 R15=000D]`,
`[op=01 R0=0049 R15=0012]`, then `[op=02 ... R15=0004]` again. The echo
demo run on the MC1 went wrong at the second pass.

## Measuring throughput (`-DGSU_MEASURE=1`)

Build with the macro, for example `cmake -DCMAKE_C_FLAGS=-DGSU_MEASURE=1 ..`.
It takes precedence over the loader and the demo. Open the USB serial port;
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
  poll, SFR high, R1), then 2 reads and 2 writes to resume (R15 get and set).
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

## STB-then-STOP hang test (`tools/hangtest/`)

`hang_test.load` is a ready-made loader stream (load address and entry
`$0200`, 226 bytes; works on both chips, since `$0200` is above the stage-1
area; the test writes to `$0400`). `hang_test_A.load` ... (`_B`, `_C`, `_G`, `_H`, `_D`) contain one phase each (use
them after a hang, because the all-in-one file stops at the first hang).
`python3 tools/hangtest/make.py` regenerates everything, with `.bin` and a
listing (`.lst.txt`) for each; `make.py BCD` builds a custom selection.

Run: build and flash the usual firmware (default for GSU-1, `-DGSU_MC1=1` for
the MC1), wait for `READY`, and send `hang_test.load` as is. Each phase prints
an uppercase letter, runs 10000 "RAM write, then STOP" iterations (the STOP is
the nop syscall), and prints the lowercase letter:

| phase | code before STOP |
|-------|------------------|
| A | `STB (R3)` directly followed by STOP |
| B | `STW (R3)` directly followed by STOP |
| C | `STB (R3)`, one NOP, STOP |
| G | `STB (R3)`, two NOPs, STOP |
| H | `STB (R3)`, three NOPs, STOP |
| D | `STB (R3)`, four NOPs, STOP |

Expected output when nothing hangs: `AaBbCcGgHhDdEND`, then the GSU waits for a key.
If a phase hangs, its lowercase letter is missing and the firmware reports
`TIMEOUT: GSU still running, SFR=0020`; reset the Pico to recover. The first
phase that does not finish shows which pattern is affected (A/B alone means
"immediately after the write"; C too means one instruction is not enough).

Result: on the GSU-1 (VCR=04, 21.4 MHz) phases A-D finish, so STB-then-STOP
does not hang there (G and H were added later and not run on it). On the MC1
(VCR=01, 10.7 MHz) A, B and C hang (`TIMEOUT ... SFR=0022`, GO still set, the
lowercase letter is missing), while G, H and D finish (G and H run twice each).
So on the MC1 both STB and STW hang when STOP follows directly or after one
NOP; two or more instructions in between are enough. The loader and stage 1
keep five instructions between STB and STOP, so they are safe.

## Verification status

Confirmed on the cart:

- Register window decode with the fixed A10-A22 levels, bus reads and writes,
  BUS_DELAY_CYCLES = 64, SFR flag bits, R0-R13, the latched-write rule.
- Cache write/readback, GO set by the R15 high byte, running from the cache
  (CBR=0), STOP leaving R15 = STOP + 2, resume by rewriting R15, at 21 MHz and
  at 10.7 MHz.
- Syscalls read, write and status (echo demo), USB serial round trip.
- The RAM loader: payload received, stored with STB, LJMP to bank $70 and
  execution from cart RAM, with the loaded program using the syscalls.
- Throughput of the syscall path (measurement build).
- The Pico SDK build, including `tusb.h` for `USE_TUD_TX_STATUS`.
- The GSU clock source is the oscillator on the cart; the chip is a GSU-1
  (VCR=04).

- The hand-assembled echo demo (`demo_prog`, +1) was assembled with the bass
  assembler by the user: the generated bytes match, and it runs on both the
  GSU-1 and the MC1. (`loader_prog` and `boot_prog` were not compared with
  assembler output; they run correctly on the cart.)
- MC1 results (diagnostics, two-stage boot, 10.7 MHz) and the GSU-1 / MC1
  comparison are in the MC1 section above and in
  `docs/cache-and-chip-notes.ja.md`.

Confirmed by the hang test: an STB or STW followed by STOP with at most one
instruction (NOP) in between hangs the MC1 but not the GSU-1; two are enough.

Nothing is left to verify.
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

# gsu-host

Pico (RP2040) firmware that drives a real Super FX cartridge as a
stand-alone processor.

Status: confirmed on a real GSU-1 cart (board 1CA6B-01).

- Diagnostic build (`-DGSU_DIAG=1`): bus reads and writes, SFR flag bits,
  R0-R13 and the latched-write rule, cache write/readback, start (GO via the
  R15 high byte), STOP with R15 = STOP + 2, and resume by rewriting R15, at
  CFGR/CLSR = 80/01 (21 MHz) and 00/00. All checks pass.
- Echo demo (`-DGSU_PROGRAM_DEMO=1`): each typed character returns +1, so the
  status, read and write syscalls work end to end over USB serial.
- Measurement build (`-DGSU_MEASURE=1`): see "Measuring throughput" for the
  numbers (about 12.4 us and 80,900 syscalls/s).
- Not yet run on the cart: the RAM loader and LJMP, and the hand-assembled
  loader program. The programs were also checked on a small simulator, which
  checks logic and branch offsets, not hardware.

VCR reads 04 on this cart. Documented values are 1 (MC1) and 4 (GSU-2); a
GSU-1 is not listed there, and web reports give 01 or 04 for it.

## Build

    export PICO_SDK_PATH=/path/to/pico-sdk
    mkdir build && cd build && cmake .. && make
    # copy gsu_host.uf2 to the Pico in BOOTSEL mode
    # demo program instead of the loader: cmake -DCMAKE_C_FLAGS=-DGSU_PROGRAM_DEMO=1 ..

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
- Speed is set by the syscall round trip. A read syscall costs 5 bus reads and
  4 bus writes (the NOP syscall's 5 + 2, plus the 2 writes that load R0), about
  16 us, so roughly 60 KB/s. This is calculated from the measured bus times;
  the loader itself has not been run or timed.
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
  hang right after a RAM write there. The loader keeps several instructions
  between STB and the next STOP. GSU-2 is not affected. Use a GSU-1/2 cart.

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

Reading the result: step 2 failing points at the bus (level shifter direction,
data lines, /WR timing); step 2 passing but step 4 failing points at GO not
starting, the cart clock or the fixed address levels; step 4 passing only with
CFGR/CLSR = 0 points at the clock configuration.

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
- Measured on the cart (125 MHz, `BUS_DELAY_CYCLES` 64, 21 MHz GSU):

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
  smallest value that still passes. Not tried yet.
- The run is silent between the two lines on purpose: printing during the
  loop would pollute the timing.
- Cross-check with a logic analyzer on /RD and /WR if you have one.

## Verification status

Confirmed on the cart:

- Register window decode with the fixed A10-A22 levels, bus reads and writes,
  BUS_DELAY_CYCLES = 64, SFR flag bits, R0-R13, the latched-write rule.
- Cache write/readback, GO set by the R15 high byte, running from the cache
  (CBR=0), STOP leaving R15 = STOP + 2, resume by rewriting R15, at 21 MHz and
  at 10.7 MHz.
- Syscalls read, write and status (echo demo), USB serial round trip.
- Throughput of the syscall path (measurement build).
- The Pico SDK build, including `tusb.h` for `USE_TUD_TX_STATUS`.
- The GSU clock source is the oscillator on the cart; the chip is a GSU-1
  (VCR=04).

Still to verify:

1. The RAM loader and LJMP (PBR = R8 low byte, R15 = Sreg, and the byte after
   LJMP executes from the old cache before the jump).
2. The hand-assembled loader program against an assembler: prefix
   combinations such as `TO Rd; FROM R0; LOB`, `FROM R4; ALT1; LJMP R8`, and
   branch offsets, which assume offsets count from the byte after the operand.
   (The demo and measurement programs run correctly on the cart.)
3. Whether the MC1's STOP/RAM-write hang applies to a GSU-1. The loader keeps
   several instructions between STB and the next STOP in any case.

## Sources

- SNES cartridge connector pinout (SNESdev Wiki): https://snes.nesdev.org/wiki/Cartridge_connector
- Fullsnes (Nocash SNES Specs), SNES Cart GSU-n chapters: https://problemkaputt.de/fullsnes.htm
- Super FX instruction set table (Yoshi's Island disassembly wiki): https://github.com/brunovalads/yoshisisland-disassembly/wiki/Super-FX-Instruction-Set
- rmstdope/neser PR 3205 (cache clearing conditions): https://github.com/rmstdope/neser/pull/3205
- sp00nznet/LakeSnes PR 2 (byte after a jump runs first): https://github.com/sp00nznet/LakeSnes/pull/2

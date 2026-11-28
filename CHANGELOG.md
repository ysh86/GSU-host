# Changelog

Newest first. "Confirmed" means run on hardware (GSU-1 = 1CA6B-01 VCR=04,
GSU-2 = 1CB5B-01 VCR=04, MC1 = 1C0N VCR=01); anything else is in the README
under "Still to verify".

## 1.4.0

- Added: GP26 (A20+A21+A22, bank $70 when High) and GP27 (/ROMSEL, Low selects the cart's
  ROM/RAM region), idle Low and High. The monitor's `d` on the MC1 now reads the RAM
  through bank $70 (A0-A14 from the existing address pins), 32 KB (`d` 0000-7FFF).
  `b` selects the path: auto (default; bank $70 on the MC1, the $6000 window on the other
  chips), `b 0` window, `b 1` bank $70 (GSU-1/GSU-2 too, to compare with the window).
  Without the wires the bank mode does not show the RAM (no warning is printed). Beyond the
  RAM's size the dump shows the mirror (MC1).
- Changed: `?` prints one command per line.
- Wiring: A20-A22 (cart pins 45-47) are joined and moved from GND to GP26; /ROMSEL (pin 49)
  moves from High to GP27. Boards without the change still work, except for bank mode.
- Confirmed on hardware: the MC1 shows its RAM in bank $70 (the loader's bytes after `l`) and
  the GSU-1 reads the same bytes there as through the window. Not run: the GSU-2.

## 1.3.1

Documentation and version only; the firmware behaves as in 1.3.0.

- Found and documented: the loader's final LJMP leaves CBR=$0040, so the cache
  window is $0040-$023F and a program at `$0200` is cached for 64 bytes only.
  Programs must start with `CACHE` ($02); then a loop of 256 NOPs runs at 1 cycle
  per instruction on the GSU-1A and the MC1 (without it 1.0 / 1.55 / 2.26 / 2.62
  cycles for 32 / 64 / 128 / 256 NOPs). This explains `CBR=0040` and the
  multiplier results that looked like ROM/RAM speed.
- Changed: the multiplier section. Cached on the GSU-1A: MULT 1, UMULT 3, FMULT 8,
  LMULT 9 cycles (MS0=0); UMULT 2, FMULT 4, LMULT 5 (MS0=1); the same cycles at
  21.4 MHz (MS0=0), i.e. half the time. UMULT, FMULT and LMULT equal the documented cycle counts (MS0=0 the larger, MS0=1 the smaller); MULT costs 2 cycles at MS0=0 and 1 at MS0=1 when isolated (1.03 / 1.00 in a run of MULTs, the second cycle overlaps the next MULT), as documented (1 or 2). The
  advice changes to `c 1` for everything on the GSU-1/GSU-2 (`m 1` only at 10.7 MHz).
  The earlier uncached table is kept for reference.
- Found on the MC1 (cached): MS0 is ignored; FMULT is 8 cycles with a nonzero operand and
  4.78 in a run where the source register is 0; short sequences of multiplies mixed with
  other instructions hang the chip (16 x MULT;NOP and 32 x FROM;ALT1;LMULT), the GSU-1A
  runs them. Causes unknown.

## 1.3.0

Verified on a GSU-1 and the MC1 (the GSU-2 was not run with the monitor and is
assumed like the GSU-1). Open: the effect of `c` / `m` is not measured, and the
GSU-1 reads CBR=0040 for a program entered at `$0200` (explained in 1.3.1).

- Changed: an unknown syscall number prints `BAD OP xx R15=xxxx` and enters the
  monitor (it used to hang the firmware). `g` and `t` are refused, `l` works.
- Documented: on the MC1, programs must not write R14 (a program using R14 ran
  old code after the first syscall; keeping the value in RAM fixed it).
- Changed (breaking): the status syscall is now `fstat` with number 28 (was
  STATUS 2). Programs that call status must use `IBT R3,#28`
  (the echo demo bytes are updated). Number 2 now stops with `BAD OP`.
- Added: syscall number 43 `times` is reserved (not implemented; `BAD OP`).
- Added: monitor command `t`: like `g`, and at the next exit it prints
  `time 12.345 ms (GSU run 12.100 ms, 34 syscalls)`: wall time from the resume
  to the exit, the time the GSU itself ran (resume to STOP, summed), and the
  syscall count. Confirmed on the MC1 and the GSU-1. The "GSU run" figure includes about 1.9 us of polling latency per syscall.

## 1.2.2

MARIO chip (MC1) reload fix.

- Fixed: on the MC1, `l` after a program had run returned at once with
  `EXIT R15=0002`. The MC1 keeps the cache window and cache lines of the program
  that ran and ignores host writes to them, so stage 1 cannot be started again.
  Now `l` after a program has run starts the loader that is still in cart RAM at
  `$0000` directly (PBR=$70, no stage 1). Reload, run, `g` and `l` again are
  confirmed on the MC1. The GSU-1 and GSU-2 still run stage 1 every time.
- Changed (convention): `$0000-$004F` holds the loader and must not be written
  by programs. Scratch RAM is `$0050-$01FF` (`LMS`/`SMS` with yy = $28 and up).
  Load and entry stay at `$0200`.
- Changed: the monitor's `r` prints PBR and CBR as `n/a` on the MC1, where they
  are not readable (they showed the stale bus value).

## 1.2.1

- Added: the monitor's `g` applies a changed clock (`c`) or multiplier (`m`)
  before it resumes after an exit. CFGR and CLSR are written only for what
  changed (`gsu_apply_config()`); `l` always writes both. Not yet run on
  hardware (README "Still to verify").

## 1.2.0

Monitor, new syscall ABI, unified loader, runtime clock and multiplier settings.

- Added: A10-A14 driven from GP21-GP25 and the cart RAM window ($6000-$7FFF):
  confirmed on the GSU-1 and GSU-2; the MC1 shows no RAM there.
- Added: exit syscall and a debug monitor: `d` (RAM dump, GSU-1/GSU-2 only), `r`,
  `g`, `l`, `c 0|1` (clock 10.7/21.4 MHz, default 10.7), `m 0|1` (multiplier
  standard/high speed, default standard; `c 1` and `m 1` refuse each other),
  `i` (banner again), `?`. The default build starts in the monitor after reset
  and loads on `l`. The start-up monitor with `l`, `i`, `c`, `m` is confirmed on
  the GSU-1 and MC1; `d`, `r`, `g` and the exit syscall still need checking.
- Added: one loader for every chip: stage 1 in cache line 0 pulls the loader
  into cart RAM at `$0000`, which LJMPs to bank `$70`. The cache-resident loader
  on the GSU-1 is gone. Payloads load at `$0200` by convention.
- Added: diagnostic [8] (RAM window), MS0 (CFGR bit 5) support, the multiplier benchmark with measurements (MS0=1 makes UMULT/FMULT/LMULT
  3-4 cycles faster on the GSU-1A at 10.7 MHz; no effect on the MC1; 21.4 MHz
  gains only 8-15 % on multiplies), board and chip wiring tables, the version
  in the banner (`GSU host v1.2.0: ...`).
- Added: an echo demo (+1) and a 7-byte exit program as loader streams.
- Changed (breaking): the syscall number is in R3 (was R1) and follows Unix:
  NOP 0, EXIT 1, STATUS 2 (unchanged), READ 3, WRITE 4. R3 is reserved for it.
  Programs built for the old ABI stop with `BAD OP`. The loader keeps its RAM
  pointer in R1; the hang test and the multiplier benchmark use R5.
- Changed (breaking): the GSU clock defaults to 10.7 MHz on every chip (21.4 MHz
  was the default except on the MC1). Use `c 1` for 21.4 MHz.
- Changed: the hang watchdog is off by default (`-DGSU_WATCHDOG_MS`).
- Removed: `GSU_MC1` (the MC1 is detected from VCR=01), `GSU_CLOCK_21MHZ`,
  `GSU_MS0`, `GSU_PROGRAM_DEMO` (the demo is now a loader stream),
  `RELOAD_CACHE_ON_RESUME`, `GSU_TRACE`, `GSU_BOOT_STAGE1`, and the optional
  GSU clock output pin.

## 1.1.0

- Changed (convention): load and enter at `$0200`, with `$0000-$01FF` as scratch
  RAM, instead of `$0100`. The hang test, examples and docs were moved.

## 1.0.0

First complete version.

- Pico (RP2040) firmware that drives a real Super FX cart as a stand-alone
  processor over the cart bus: register window, cache loading, GO/STOP handling,
  and syscalls over USB serial (read, write, status, nop).
- A RAM loader that takes a payload over USB serial; MC1 support (10.7 MHz, a
  two-stage boot because the MC1 runs host-written code from one cache block
  only).
- Diagnostic build, throughput measurement build, the STB-then-STOP hang test
  (hangs on the MC1, not on the GSU-1), and
  `docs/cache-and-chip-notes.ja.md`.

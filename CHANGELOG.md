# Changelog

Newest first. "Confirmed" means run on hardware (GSU-1 = 1CA6B-01 VCR=04,
GSU-2 = 1CB5B-01 VCR=04, MC1 = 1C0N VCR=01); anything else is in the README
under "Still to verify".

## 1.3.0

Verified on a GSU-1 and the MC1 (the GSU-2 was not run with the monitor and is
assumed like the GSU-1). Open: the effect of `c` / `m` is not measured, and the
GSU-1 reads CBR=0040 for a program entered at `$0200` (README "Still to verify").

- Changed (breaking): the status syscall is now `fstat` with number 28 (was
  STATUS 2). Programs that call status must use `IBT R3,#28`
  (`tools/echo_demo.load` is updated). Number 2 now stops with `BAD OP`.
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
- Added: diagnostic [8] (RAM window), MS0 (CFGR bit 5) support, the multiplier
  benchmark `tools/multest/` with measurements (MS0=1 makes UMULT/FMULT/LMULT
  3-4 cycles faster on the GSU-1A at 10.7 MHz; no effect on the MC1; 21.4 MHz
  gains only 8-15 % on multiplies), board and chip wiring tables, the version
  in the banner (`GSU host v1.2.0: ...`).
- Added: `tools/echo_demo.load` (echo +1) and `tools/exit_test.load`.
- Changed (breaking): the syscall number is in R3 (was R1) and follows Unix:
  NOP 0, EXIT 1, STATUS 2 (unchanged), READ 3, WRITE 4. R3 is reserved for it.
  Programs built for the old ABI stop with `BAD OP`. The loader keeps its RAM
  pointer in R1; the hang test and the multiplier benchmark use R5.
- Changed (breaking): the GSU clock defaults to 10.7 MHz on every chip (21.4 MHz
  was the default except on the MC1). Use `c 1` for 21.4 MHz.
- Changed: the hang watchdog is off by default (`-DGSU_WATCHDOG_MS`).
- Removed: `GSU_MC1` (the MC1 is detected from VCR=01), `GSU_CLOCK_21MHZ`,
  `GSU_MS0`, `GSU_PROGRAM_DEMO` (the demo moved to `tools/echo_demo.load`),
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
  (hangs on the MC1, not on the GSU-1), a simulator-based `tools/check.sh`, and
  `docs/cache-and-chip-notes.ja.md`.

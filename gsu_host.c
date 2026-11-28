// gsu_host.c — RP2040 (Pico 1) host for a real Super FX (GSU) cartridge.
//
// The Pico plays the role of the S-CPU. It drives the GSU register window
// ($3000-$32FF) and, for the debug monitor, reads the cart RAM window
// ($6000-$7FFF = RAM $0000-$1FFF) while the GSU is stopped. It loads a 15-byte
// stage-1 program into the instruction cache; that pulls the RAM loader (served
// by the firmware) into cart RAM at $0000 and jumps to it. STOP is a system
// call into USB serial.
//
// ABI (GSU side): R3 low byte = op, R0 low byte = data, then STOP + dummy byte.
// The numbers follow Unix where there is a counterpart.
//   op 0  nop     nothing (used by the measurement build)
//   op 1  exit    control returns to the Pico: prints EXIT and enters the
//                 debug monitor (d dump RAM, r registers, g resume, l load)
//   op 3  read    blocks until a byte arrives; R0 <- byte (zero-extended)
//   op 4  write   R0 low -> USB serial
//   op 28 fstat   R0 <- 6850-ACIA-style status (zero-extended), bit0 = RDRF
//                 (receive data available), bit1 = TDRE (can transmit)
//   op 43 times   reserved (not implemented: stops with BAD OP)
//
// STOP semantics (fullsnes, GSU-n chapter): STOP clears GO, sets IRQ (even if
// masked) and leaves R15 = STOP address + 2. The byte after STOP is
// prefetched but NOT executed, so it is a dummy and resume skips it.
// The code cache is NOT cleared by STOP; it is cleared by CACHE, LJMP, or a
// host write of GO=0 to SFR (which also sets CBR=0).
//
// STATUS: confirmed on hardware, on a GSU-1 and on a MARIO chip (MC1): bus,
// registers, cache, start/STOP/resume, syscalls and the RAM loader (see the
// README). The STB-then-STOP hang (fullsnes) was reproduced on the MC1 and is
// absent on the GSU-1. On the MC1, 0 or 1 NOP between the write
// and STOP hangs, 2 or more are fine.
// A10-A14 on GP21-GP25 and the RAM window (diag [8], host write/readback) are
// confirmed on a GSU-1 and a GSU-2; on the MC1 (board 1C0N) the window neither
// reads nor writes RAM (diag [8].2-.5), so on the MC1 (detected by VCR=01) the monitor's
// "d" reads through bank $70 instead (GP26 = A20-A22, GP27 = /ROMSEL; confirmed). Confirmed on a GSU-1 and the MC1: the monitor (l, i, c,
// m, r, g, t, the exit syscall) and the loader running from cart RAM at $0000; "d"
// through the window on the GSU-1. The GSU-2 was not run with the monitor (assumed like the GSU-1).
// Not measured (marked [VERIFY]): that c/m really change the clock and MS0.

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "tusb.h"

// ---------------------------------------------------------------- pins ----
// All cart pins are 5 V logic: use level shifters / 5 V-tolerant buffers.
#define PIN_A0     0               // A0..A9  -> GP0..GP9
#define A_MASK     (0x3FFu << PIN_A0)
#define PIN_D0     10              // D0..D7  -> GP10..GP17
#define D_MASK     (0xFFu << PIN_D0)
#define PIN_RD     18              // /RD
#define PIN_WR     19              // /WR
#define PIN_RESET  20              // /RESET
#define PIN_A10    21              // A10..A14 -> GP21..GP25
#define AH_MASK    (0x1Fu << PIN_A10)
#define PIN_BANK   26              // A20+A21+A22 (cart pins 45-47, joined): High = bank $70
#define PIN_ROMSEL 27              // /ROMSEL (/CART, cart pin 49): Low = ROM/RAM region selected
#define SEL_MASK   ((1u << PIN_BANK) | (1u << PIN_ROMSEL))

// A10..A14 are driven by the Pico: bit 0 = A10 ... bit 4 = A14.
//   AH_REGS: A12=1, A13=1 (A10, A11, A14 = 0)  -> $3000-$33FF, the GSU registers
//   AH_RAM : A13=1, A14=1, A12..A10 = block    -> $6000-$7FFF, cart RAM window
// A15, A16-A19 and A23 must be hard-wired low. A20-A22 (joined) and /ROMSEL are driven
// by GP26 and GP27 (idle: A20-A22 low, /ROMSEL high, so the cart decodes bank $00 as
// with the earlier hard-wired levels; a board without these two wires is unaffected).
// Only the monitor's RAM dump through bank $70 (`b`, the MC1 default) uses them.
// A23 is connected on only one of the three inspected boards (README, "Boards and chips").
// There is no clock output: a cart that needs SYSTEM CLK gets an external
// oscillator (the only RP2040 clock output pins, GP21/23/24/25, are the A10-A14
// lines now).
#define AH_REGS    0x0C
#define AH_RAM     0x18

// Strobe/setup delay in system-clock cycles (64 = ~0.5 us at 125 MHz).
// Increase if reads are flaky.
#define BUS_DELAY_CYCLES   64

// TDRE uses the TinyUSB CDC write FIFO. Set to 0 to report TDRE = "USB
// connected" using only the stdio API.
#define GSU_HOST_VERSION "1.4.0"

#ifndef USE_TUD_TX_STATUS
#define USE_TUD_TX_STATUS  1
#endif

// ---------------------------------------------------- GSU register map ----
#define GSU_R(n)    (0x3000 + 2 * (n))   // R0..R15, 16-bit little endian
#define GSU_SFR_L   0x3030
#define GSU_SFR_H   0x3031
#define GSU_PBR     0x3034
#define GSU_CFGR    0x3037
#define GSU_CLSR    0x3039
#define GSU_SCMR    0x303A
#define GSU_VCR     0x303B
#define GSU_CBR_L   0x303E               // read-only cache base register
#define GSU_CBR_H   0x303F
#define GSU_CACHE   0x3100               // 512-byte instruction cache

#define SFR_GO      0x20                 // bit 5 of SFR low byte
#define SCMR_RAN    0x08
#define SCMR_RON    0x10
#define SCMR_NORMAL (SCMR_RAN | SCMR_RON)  // GSU owns ROM and RAM (normal running)
#define CFGR_IRQOFF 0x80                 // bit7 = IRQ mask (we poll GO instead).
#define CFGR_MS0    0x20                 // bit5 = MS0, multiplier speed (0 standard, 1 high)
                                         // NOT 0x20: that is MS0 (multiplier high
                                         // speed), which must be 0 with CLSR=1.
// GSU clock and multiplier speed are runtime settings of the monitor ("c 0|1",
// "m 0|1"), applied when the loader is loaded ("l") and, if changed, at "g". Defaults:
// 10.7 MHz, standard multiplier. 21.4 MHz runs away on the MC1 in longer programs
// (README); MS0 is for the 10.7 MHz clock only (docs) and measured to speed up
// UMULT/FMULT/LMULT by 3-4 cycles on the GSU-1A, no effect on the MC1 (README
// "Multiplier speed"; the GSU-2 is assumed like the GSU-1A, not measured).
static bool opt_clk21 = false;           // c 1: CLSR = 1 (21.4 MHz)
static bool opt_ms0 = false;             // m 1: CFGR bit 5 (multiplier high speed)
static bool applied_valid = false;       // CFGR/CLSR written at least once (by "l")
static bool applied_clk21, applied_ms0;  // what is in the chip now (CFGR cannot be read back)
#define DIAG_CLSR   0x01                 // the diagnostics run at 21 MHz ("normal config")

// System call numbers (R3 low byte; Unix numbering where there is a counterpart)
#define SYS_NOP     0                    // does nothing; used by the MEASURE build
#define SYS_EXIT    1                    // control returns to the Pico (monitor)
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_FSTAT   28                   // serial status -> R0
#define SYS_TIMES   43                   // reserved, not implemented

// Watchdog: -DGSU_WATCHDOG_MS=500 prints "TIMEOUT: GSU still running" into the
// output every 500 ms while the GSU runs without a syscall. Off by default (0): a
// program may legitimately compute for seconds, and the message would land in the
// binary stream. Turn it on to see hangs (STB-then-STOP on the MC1, MC1 21 MHz runaway).
#ifndef GSU_WATCHDOG_MS
#define GSU_WATCHDOG_MS 0
#endif

// Throughput measurement build (-DGSU_MEASURE=1). The GSU calls the NOP
// syscall in a tight loop; the host times MEASURE_COUNT of them and prints
// one report over USB, then halts. No USB data traffic during the run, so
// the result is the bus + polling cost per syscall.
#ifndef GSU_MEASURE
#define GSU_MEASURE 0
#endif
#ifndef GSU_DIAG
#define GSU_DIAG 0
#endif

// The loader build (default) always boots in two stages: the cache holds only
// the 15-byte stage-1 program, which pulls the loader into cart RAM at $0000 and
// jumps to it. Not used by the measure build, which runs from the cache.
#define BOOT_ACTIVE (!GSU_MEASURE)

#ifndef MEASURE_COUNT
#define MEASURE_COUNT 1000000u
#endif

// ----------------------------------------------------------- bus layer ----
static inline void bus_delay(void) { busy_wait_at_least_cycles(BUS_DELAY_CYCLES); }

static void bus_init(void) {
    const uint32_t ctl = (1u << PIN_RD) | (1u << PIN_WR) | (1u << PIN_RESET);
    gpio_init_mask(A_MASK | AH_MASK | D_MASK | ctl | SEL_MASK);
    // A0-A9 = 0, A10-A14 = register window, strobes inactive (high)
    gpio_put_masked(A_MASK | AH_MASK | ctl, ((uint32_t)AH_REGS << PIN_A10) | ctl);
    gpio_set_dir_out_masked(A_MASK | AH_MASK | ctl);
    // bank $00 (A20-A22 low), /ROMSEL high
    gpio_put_masked(SEL_MASK, 1u << PIN_ROMSEL);
    gpio_set_dir_out_masked(SEL_MASK);
    gpio_set_dir_in_masked(D_MASK);
}

static void reg_write(uint16_t reg, uint8_t v) {   // A10-A14 must be at AH_REGS
    gpio_put_masked(A_MASK, (uint32_t)(reg & 0x3FF) << PIN_A0);
    gpio_put_masked(D_MASK, (uint32_t)v << PIN_D0);
    gpio_set_dir_out_masked(D_MASK);
    bus_delay();
    gpio_put(PIN_WR, 0);
    bus_delay();
    gpio_put(PIN_WR, 1);
    bus_delay();
    gpio_set_dir_in_masked(D_MASK);
}

static uint8_t reg_read(uint16_t reg) {
    gpio_put_masked(A_MASK, (uint32_t)(reg & 0x3FF) << PIN_A0);
    bus_delay();
    gpio_put(PIN_RD, 0);
    bus_delay();
    uint8_t v = (uint8_t)((gpio_get_all() & D_MASK) >> PIN_D0);
    gpio_put(PIN_RD, 1);
    bus_delay();
    return v;
}

static void cart_reset(void) {
    gpio_put(PIN_RESET, 0);
    sleep_ms(10);
    gpio_put(PIN_RESET, 1);
    sleep_ms(10);
}

// ------------------------------------------------------- cart RAM window ----
// Confirmed on a GSU-1 and a GSU-2. With A13=A14=1 and A15=0 the cart maps its RAM at
// $6000-$7FFF in bank $00: 8 KB = RAM $0000-$1FFF (the first 8 KB of the RAM the
// GSU sees at $70:0000). A0-A9 come from GP0-GP9 and A10-A12 from GP21-GP23, so
// the Pico can reach all 8 KB. The GSU must be stopped (GO=0) and SCMR.RAN must
// give the RAM to the S-CPU for the duration; both functions set RAN=0 and put
// SCMR_NORMAL back. The address lines return to the register window levels, so
// reg_read()/reg_write() can be used straight afterwards.
static bool is_mc1(void);
#define RAM_WINDOW_SIZE 0x2000
// Bank $70 mode (monitor `b`): A20-A22 High, /ROMSEL Low, A15 Low, so A0-A14 address
// the 32 KB at $70:0000-$7FFF directly. Needs GP26 and GP27 wired (README). Confirmed on the MC1 and the GSU-1
// (b 0 and b 1 agree); the GSU-2 was not run with it.
#define RAM_BANK_SIZE   0x8000

static inline uint32_t ram_addr_bits(uint16_t off) {
    return ((uint32_t)(off & 0x3FF) << PIN_A0) |
           ((uint32_t)(AH_RAM | ((off >> 10) & 7)) << PIN_A10);
}

static inline uint32_t ram_bank_addr_bits(uint16_t off) {   // A0-A14 = off, in bank $70
    return ((uint32_t)(off & 0x3FF) << PIN_A0) | ((uint32_t)((off >> 10) & 0x1F) << PIN_A10);
}

// Reads up to n bytes from RAM offset off; returns how many were read
// (clamped at the end of the window, or of the 32 KB with bank=true).
static uint16_t ram_read_ex(bool bank, uint16_t off, uint8_t *buf, uint16_t n) {
    const uint16_t size = bank ? RAM_BANK_SIZE : RAM_WINDOW_SIZE;
    if (off >= size) return 0;
    if (n > size - off) n = size - off;
    reg_write(GSU_SCMR, SCMR_RON);               // RAN=0: RAM belongs to the S-CPU
    if (bank) { gpio_put_masked(SEL_MASK, 1u << PIN_BANK); bus_delay(); }   // bank $70, /ROMSEL low
    for (uint16_t i = 0; i < n; i++) {
        gpio_put_masked(A_MASK | AH_MASK, bank ? ram_bank_addr_bits(off + i) : ram_addr_bits(off + i));
        bus_delay();
        gpio_put(PIN_RD, 0);
        bus_delay();
        buf[i] = (uint8_t)((gpio_get_all() & D_MASK) >> PIN_D0);
        gpio_put(PIN_RD, 1);
        bus_delay();
    }
    gpio_put_masked(A_MASK | AH_MASK, (uint32_t)AH_REGS << PIN_A10);
    if (bank) { gpio_put_masked(SEL_MASK, 1u << PIN_ROMSEL); bus_delay(); }   // back to bank $00, /ROMSEL high
    reg_write(GSU_SCMR, SCMR_NORMAL);
    return n;
}
static uint16_t ram_read(uint16_t off, uint8_t *buf, uint16_t n) { return ram_read_ex(false, off, buf, n); }

#if GSU_DIAG
// Only the diagnostic build writes through the window (the monitor is read-only).
static void ram_write(uint16_t off, uint8_t v) {
    if (off >= RAM_WINDOW_SIZE) return;
    reg_write(GSU_SCMR, SCMR_RON);
    gpio_put_masked(A_MASK | AH_MASK, ram_addr_bits(off));
    gpio_put_masked(D_MASK, (uint32_t)v << PIN_D0);
    gpio_set_dir_out_masked(D_MASK);
    bus_delay();
    gpio_put(PIN_WR, 0);
    bus_delay();
    gpio_put(PIN_WR, 1);
    bus_delay();
    gpio_set_dir_in_masked(D_MASK);
    gpio_put_masked(A_MASK | AH_MASK, (uint32_t)AH_REGS << PIN_A10);
    reg_write(GSU_SCMR, SCMR_NORMAL);
}
#endif

// ------------------------------------------------------- GSU program ------
// RAM loader (the program every build boots into). Protocol over USB serial,
// all little endian:
//   addr(2)  load address in cart RAM  (RAM bank 0 = $70)
//   entry(2) address to jump to afterwards
//   len(2)   number of payload bytes, must be >= 1
//   payload  len bytes
// The loader reads each byte with the read syscall, stores it with STB,
// then does LJMP to bank $70 at `entry`. LJMP empties the cache, which then
// refills from RAM. The loaded program keeps using the same syscalls.
//
// Each 16-bit read is a 16-byte macro (d = 1, 4, 2 for addr, entry, len):
//   A3 03        IBT R3,#3        ; op = read
//   00 01        STOP + dummy     ; R0 low = byte
//   1d B0 9E     TO Rd; FROM R0; LOB          ; Rd = low byte
//   A3 03 00 01  IBT R3,#3; STOP + dummy
//   9E 4D        LOB; SWAP        ; R0 = high byte << 8
//   1d B0 Cd     TO Rd; FROM R0; OR Rd        ; Rd |= R0
// then the copy loop:
//   30: A3 03 00 01   IBT R3,#3; STOP + dummy
//   34: 3D 31         STB (R1)    ; ALT1 + store byte
//   36: D1            INC R1
//   37: E2            DEC R2      ; sets Z
//   38: 08 F6 01      BNE $30; NOP (delay slot)
//   3B: A8 70         IBT R8,#$70
//   3D: B4 3D 98 01   FROM R4; ALT1; LJMP R8; NOP (delay slot)
// Runs from cart RAM $0000-$004F on every chip, entered through the stage-1 boot
// (boot_prog below). Not compared with assembler output.
static const uint8_t loader_prog[80] __attribute__((unused)) = {
    // addr -> R1
    0xA3, 0x03, 0x00, 0x01, 0x11, 0xB0, 0x9E, 0xA3,
    0x03, 0x00, 0x01, 0x9E, 0x4D, 0x11, 0xB0, 0xC1,
    // entry -> R4
    0xA3, 0x03, 0x00, 0x01, 0x14, 0xB0, 0x9E, 0xA3,
    0x03, 0x00, 0x01, 0x9E, 0x4D, 0x14, 0xB0, 0xC4,
    // len -> R2
    0xA3, 0x03, 0x00, 0x01, 0x12, 0xB0, 0x9E, 0xA3,
    0x03, 0x00, 0x01, 0x9E, 0x4D, 0x12, 0xB0, 0xC2,
    // copy loop, then LJMP
    0xA3, 0x03, 0x00, 0x01, 0x3D, 0x31, 0xD1, 0xE2,
    0x08, 0xF6, 0x01, 0xA8, 0x70, 0xB4, 0x3D, 0x98,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
};

// Measurement program: calls the NOP syscall forever.
//  00: A3 00     IBT R3,#0        ; op = nop
//  02: 00        STOP
//  03: 01        (dummy)
//  04: 05 FA     BRA $00          ; -6 (base = byte after operand)
//  06: 01        NOP              ; branch delay slot
static const uint8_t measure_prog[16] __attribute__((unused)) = {
    0xA3, 0x00, 0x00, 0x01, 0x05, 0xFA, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
};

// Stage-1 boot, used on every chip. On the MARIO chip only cache line 0
// ($00-$0F) is reliably executed when the host writes it (see the README, MC1
// section), so this 15-byte program fits in line 0. The host
// presets R2 = byte count (sizeof loader_prog), R1 = 0 (RAM address), R4 = 0
// (jump target) and R8 = $70 (RAM bank) before starting it. It reads bytes with
// the read syscall (served from loader_prog by the firmware, not from USB),
// stores them in RAM with STB, then LJMPs to $70:0000, where the normal loader
// runs from cart RAM.
//  00: A3 03     IBT R3,#3        ; op = read
//  02: 00 01     STOP + dummy     ; R0 = next byte
//  04: 3D 31     STB (R1)
//  06: D1        INC R1
//  07: E2        DEC R2           ; sets Z
//  08: 08 F6     BNE $00          ; -10 from the byte after the operand
//  0A: 01        NOP              ; branch delay slot
//  0B: B4        FROM R4
//  0C: 3D 98     ALT1; LJMP R8    ; PBR = $70, R15 = R4 = 0
//  0E: 01        NOP              ; LJMP delay slot
//  0F: 01        padding
static const uint8_t boot_prog[16] __attribute__((unused)) = {
    0xA3, 0x03, 0x00, 0x01, 0x3D, 0x31, 0xD1, 0xE2,
    0x08, 0xF6, 0x01, 0xB4, 0x3D, 0x98, 0x01, 0x01,
};

// Select the cache program: stage-1 boot (default; it pulls the loader into
// cart RAM) or the measurement loop (-DGSU_MEASURE=1).
#if GSU_MEASURE
#define gsu_prog measure_prog
#else
#define gsu_prog boot_prog
#endif

// Cache loading, per fullsnes: write SFR=0000h (GO=0) first; this forces
// CBR=0 and marks every cache line empty. Then write the code in 16-byte
// lines; writing a line's last byte ($xxxF) marks that line valid. CBR is
// read-only from the host.
static void load_program(void) {
    reg_write(GSU_SFR_L, 0x00);
    reg_write(GSU_SFR_H, 0x00);
    for (uint16_t i = 0; i < sizeof gsu_prog; i++)
        reg_write(GSU_CACHE + i, gsu_prog[i]);
}

// Host writes to R0..R15 are latched (fullsnes): a write to the even address
// (LSB) only sets LATCH; the write to the odd address (MSB) then applies
// LSB=LATCH and MSB=data. A register therefore changes only when its MSB is
// written, so always write LSB first and then MSB.
static void gsu_reg16_write(uint8_t n, uint16_t v) {
    reg_write(GSU_R(n),     v & 0xFF);
    reg_write(GSU_R(n) + 1, v >> 8);
}

static uint16_t gsu_reg16_read(uint8_t n) {
    uint16_t lo = reg_read(GSU_R(n));
    uint16_t hi = reg_read(GSU_R(n) + 1);
    return lo | (hi << 8);
}

static void set_r15(uint16_t pc) {
    gsu_reg16_write(15, pc);                 // the MSB write also sets GO and starts the GSU
}

static uint16_t get_r15(void) {
    uint16_t lo = reg_read(GSU_R(15));
    uint16_t hi = reg_read(GSU_R(15) + 1);
    return lo | (hi << 8);
}

// ---------------------------------------------- USB serial (syscall side) --
static int rx_hold = -1;                     // one-byte peek buffer

static bool rx_ready(void) {
    if (rx_hold < 0) {
        int c = getchar_timeout_us(0);
        if (c >= 0) rx_hold = c;
    }
    return rx_hold >= 0;
}

static uint8_t rx_get(void) {
    if (rx_hold >= 0) {
        uint8_t c = (uint8_t)rx_hold;
        rx_hold = -1;
        return c;
    }
    return (uint8_t)getchar();               // blocks
}

#if BOOT_ACTIVE
static const uint8_t *boot_src;
static uint16_t boot_left;                   // bytes of loader_prog still to serve
#endif

static bool tx_ready(void) {
    if (!stdio_usb_connected()) return false;
#if USE_TUD_TX_STATUS
    return tud_cdc_write_available() > 0;
#else
    return true;
#endif
}

// 6850-ACIA-style low status bits: bit0 = RDRF, bit1 = TDRE.
static uint8_t sys_status(void) {
    return (rx_ready() ? 0x01 : 0x00) | (tx_ready() ? 0x02 : 0x00);
}

// ------------------------------------------------------------ measuring ----
#if GSU_MEASURE
static uint32_t meas_polls;                  // SFR reads while waiting for STOP
#endif

static inline bool gsu_running(void) {
#if GSU_MEASURE
    meas_polls++;
#endif
    return reg_read(GSU_SFR_L) & SFR_GO;
}

#if GSU_MEASURE
// Time raw bus accesses before the GSU is started.
static void measure_bus(void) {
    const uint32_t n = 10000;
    uint64_t t0 = time_us_64();
    for (uint32_t i = 0; i < n; i++) (void)reg_read(GSU_SFR_L);
    uint64_t t1 = time_us_64();
    for (uint32_t i = 0; i < n; i++) reg_write(GSU_R(13), 0);   // R13 is unused
    uint64_t t2 = time_us_64();
    printf("MEASURE bus: read=%u ns, write=%u ns (sys clock %u Hz, BUS_DELAY_CYCLES=%d)\r\n",
           (unsigned)((t1 - t0) * 1000 / n), (unsigned)((t2 - t1) * 1000 / n),
           (unsigned)clock_get_hz(clk_sys), BUS_DELAY_CYCLES);
}

static void report_measure(uint64_t total_us) {
    printf("\r\nMEASURE syscalls: n=%u total=%llu us avg=%u ns rate=%u /s polls_x100=%u\r\n",
           (unsigned)MEASURE_COUNT, (unsigned long long)total_us,
           (unsigned)(total_us * 1000 / MEASURE_COUNT),
           (unsigned)((uint64_t)MEASURE_COUNT * 1000000 / total_us),
           (unsigned)((uint64_t)meas_polls * 100 / MEASURE_COUNT));
}
#endif


// ---------------------------------------------------------- diagnostics ----
// -DGSU_DIAG=1: after reset, run a series of checks over USB serial and print
// PASS/FAIL lines, then wait for a key to run them again. Text only.
//  1. identity: VCR, SFR, CBR
//  2. R0-R13 write/readback patterns, address aliasing, latch behaviour
//  3. cache write/readback ($3100-$32FF) with bit-level mismatch report
//  4. start test: a tiny program (IBT R1,#5A; STOP; IBT R2,#77; STOP) is run
//     twice, once with CFGR/CLSR as in normal use and once with both 0
//  5-7. clock check, cache line tests, last-block test (see below)
//  8. cart RAM window write/readback through A10-A14
// R14 (ROM read trigger) and R15 (starts the GSU) are not touched by 2.
#if GSU_DIAG
static int diag_fail;

static void d_check(const char *what, bool ok) {
    printf("  %-44s %s\r\n", what, ok ? "PASS" : "FAIL");
    if (!ok) diag_fail++;
}

static uint16_t d_rd16(uint8_t n) {
    uint16_t lo = reg_read(GSU_R(n));
    uint16_t hi = reg_read(GSU_R(n) + 1);
    return lo | (hi << 8);
}

static void diag_regs(void) {
    printf("[2] register write/readback (R0-R13)\r\n");
    {   // SFR flag bits 1-4 are host-writable while GO=0: a write test that
        // does not involve R0-R15.
        static const uint8_t v[] = {0x1E, 0x00, 0x0A, 0x14};
        int sbad = 0;
        for (unsigned k = 0; k < sizeof v; k++) {
            reg_write(GSU_SFR_L, v[k]);
            uint8_t r = reg_read(GSU_SFR_L) & 0x1E;
            if (r != v[k]) { printf("    SFR wrote %02X read %02X\r\n", v[k], r); sbad++; }
        }
        reg_write(GSU_SFR_L, 0x00);
        d_check("SFR flag bits write/readback", sbad == 0);
    }
    static const uint16_t pat[] = {0x0000, 0xFFFF, 0x5555, 0xAAAA, 0x1234, 0xFEDC};
    int bad = 0;
    for (uint8_t n = 0; n < 14; n++) {
        for (unsigned k = 0; k < sizeof pat / sizeof pat[0]; k++) {
            gsu_reg16_write(n, pat[k]);
            uint16_t r = d_rd16(n);
            if (r != pat[k]) {
                printf("    R%u wrote %04X read %04X (xor %04X)\r\n",
                       n, pat[k], r, pat[k] ^ r);
                bad++;
            }
        }
    }
    d_check("patterns 0000/FFFF/5555/AAAA/1234/FEDC", bad == 0);

    bad = 0;
    for (uint8_t n = 0; n < 14; n++) gsu_reg16_write(n, 0x1000 + n * 0x0101);
    for (uint8_t n = 0; n < 14; n++) {
        uint16_t r = d_rd16(n);
        if (r != 0x1000 + n * 0x0101) {
            printf("    R%u expected %04X read %04X\r\n", n, 0x1000 + n * 0x0101, r);
            bad++;
        }
    }
    d_check("unique value per register (address lines)", bad == 0);

    gsu_reg16_write(2, 0x1111);
    reg_write(GSU_R(2), 0x22);                 // LSB only: must not change R2
    uint16_t a = d_rd16(2);
    reg_write(GSU_R(2) + 1, 0x33);             // MSB applies LSB=LATCH
    uint16_t b = d_rd16(2);
    printf("    LSB-only write: R2=%04X (expect 1111); then MSB: R2=%04X (expect 3322)\r\n", a, b);
    d_check("latched write (LSB then MSB)", a == 0x1111 && b == 0x3322);
}

static void diag_cache(void) {
    printf("[3] cache write/readback ($3100-$32FF)\r\n");
    reg_write(GSU_SFR_L, 0);
    reg_write(GSU_SFR_H, 0);
    for (uint16_t i = 0; i < 512; i++)
        reg_write(GSU_CACHE + i, (uint8_t)(i * 7 + 3 + (i >> 8)));
    int bad = 0, shown = 0;
    uint8_t xor_all = 0;
    for (uint16_t i = 0; i < 512; i++) {
        uint8_t e = (uint8_t)(i * 7 + 3 + (i >> 8));
        uint8_t r = reg_read(GSU_CACHE + i);
        if (r != e) {
            bad++;
            xor_all |= r ^ e;
            if (shown++ < 8)
                printf("    $%04X wrote %02X read %02X\r\n", GSU_CACHE + i, e, r);
        }
    }
    printf("    mismatches=%d, differing data bits mask=%02X\r\n", bad, xor_all);
    d_check("cache readback matches", bad == 0);
    if (bad)
        printf("    note: cache may simply not be host-readable; the start test decides.\r\n");
}

// Returns true if both STOPs were reached with the expected results.
static bool diag_start(const char *label, uint8_t cfgr, uint8_t clsr) {
    static const uint8_t p[16] = {0xA1, 0x5A, 0x00, 0x01, 0xA2, 0x77, 0x00, 0x01,
                                  0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
    printf("[4] start test: %s (CFGR=%02X CLSR=%02X)\r\n", label, cfgr, clsr);
    reg_write(GSU_CFGR, cfgr);
    reg_write(GSU_CLSR, clsr);
    reg_write(GSU_SCMR, SCMR_NORMAL);
    reg_write(GSU_PBR, 0);
    reg_write(GSU_SFR_L, 0);
    reg_write(GSU_SFR_H, 0);
    for (uint8_t i = 0; i < 16; i++) reg_write(GSU_CACHE + i, p[i]);
    gsu_reg16_write(1, 0);
    gsu_reg16_write(2, 0);

    set_r15(0);
    uint8_t sfr_now = reg_read(GSU_SFR_L);
    uint64_t t0 = time_us_64();
    bool done = false;
    while (time_us_64() - t0 < 100000)
        if (!(reg_read(GSU_SFR_L) & SFR_GO)) { done = true; break; }
    uint32_t us = (uint32_t)(time_us_64() - t0);
    uint8_t sh = reg_read(GSU_SFR_H), sl = reg_read(GSU_SFR_L);
    uint16_t r1 = d_rd16(1), r15 = get_r15();
    printf("    SFR right after start=%02X; after wait SFR=%02X%02X (%u us), R1=%04X R15=%04X\r\n",
           sfr_now, sh, sl, us, r1, r15);
    bool first = done && r1 == 0x005A && r15 == 0x0004;
    d_check("GSU finished (GO=0 within 100 ms)", done);
    d_check("IBT executed (R1=005A), R15=0004", first);
    if (!first) return false;

    set_r15(get_r15());
    done = false;
    t0 = time_us_64();
    while (time_us_64() - t0 < 100000)
        if (!(reg_read(GSU_SFR_L) & SFR_GO)) { done = true; break; }
    uint16_t r2 = d_rd16(2);
    r15 = get_r15();
    printf("    after resume: R2=%04X R15=%04X\r\n", r2, r15);
    bool second = done && r2 == 0x0077 && r15 == 0x0008;
    d_check("resume via R15 rewrite (R2=0077, R15=0008)", second);
    return second;
}


// [5] clock check: the same cache-resident loop (DEC R2; BNE; NOP, 3 instructions
// per iteration) is timed with CLSR=0 and CLSR=1. If CLSR takes effect the
// second run is about twice as fast; if the chip ignores it the times match.
static uint32_t diag_loop_us(uint8_t clsr, uint16_t *r2_after) {
    static const uint8_t p[16] = {0xE2, 0x08, 0xFD, 0x01, 0x00, 0x01, 0x01, 0x01,
                                  0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
    reg_write(GSU_CFGR, CFGR_IRQOFF);
    reg_write(GSU_CLSR, clsr);
    reg_write(GSU_SFR_L, 0);
    reg_write(GSU_SFR_H, 0);
    for (uint8_t i = 0; i < 16; i++) reg_write(GSU_CACHE + i, p[i]);
    gsu_reg16_write(2, 0xFFFF);                // 65535 iterations
    uint64_t t0 = time_us_64();
    set_r15(0);
    while ((reg_read(GSU_SFR_L) & SFR_GO) && time_us_64() - t0 < 2000000) {}
    uint32_t us = (uint32_t)(time_us_64() - t0);
    (void)reg_read(GSU_SFR_H);
    *r2_after = d_rd16(2);
    return us;
}

static void diag_clock(void) {
    printf("[5] clock check: 65535 x (DEC; BNE; NOP) from the cache\r\n");
    uint16_t ra, rb;
    uint32_t slow = diag_loop_us(0x00, &ra);
    uint32_t fast = diag_loop_us(0x01, &rb);
    printf("    CLSR=00: %u us (R2=%04X)   CLSR=01: %u us (R2=%04X)\r\n",
           (unsigned)slow, ra, (unsigned)fast, rb);
    d_check("loop finished (R2=0000 both runs)", ra == 0 && rb == 0);
    if (fast == 0) return;
    unsigned ratio = (unsigned)((uint64_t)slow * 100 / fast);
    printf("    ratio slow/fast = %u.%02u (about 2.00 if CLSR=1 gives 21 MHz)\r\n",
           ratio / 100, ratio % 100);
    printf("    approx. instruction rate: %u kHz at CLSR=00, %u kHz at CLSR=01\r\n",
           (unsigned)(196605000u / (slow ? slow : 1)), (unsigned)(196605000u / fast));
    if (ratio >= 170) printf("    -> CLSR=01 takes effect (about 21 MHz).\r\n");
    else if (ratio >= 90 && ratio <= 115) printf("    -> no speed change: CLSR=01 has no effect on this chip.\r\n");
    else printf("    -> unclear; check the numbers.\r\n");
}


// [6] cache line tests: run small programs from specific cache lines and from
// across a line boundary, to see whether the GSU keeps using host-written lines.
struct lt { const char *name; uint16_t base; uint8_t code[32]; uint8_t nlines;
            uint16_t start, e1, e2, e15, e1b, e2b, e15b; bool has2; uint16_t e3; };

static bool diag_line_test(const struct lt *t) {
    printf("    %s\r\n", t->name);
    reg_write(GSU_CFGR, CFGR_IRQOFF);
    reg_write(GSU_CLSR, DIAG_CLSR);
    reg_write(GSU_SFR_L, 0);
    reg_write(GSU_SFR_H, 0);
    for (uint8_t i = 0; i < t->nlines * 16; i++) reg_write(GSU_CACHE + t->base + i, t->code[i]);
    gsu_reg16_write(1, 0);
    gsu_reg16_write(2, 0);
    gsu_reg16_write(3, 0);
    set_r15(t->start);
    uint64_t t0 = time_us_64();
    while ((reg_read(GSU_SFR_L) & SFR_GO) && time_us_64() - t0 < 100000) {}
    (void)reg_read(GSU_SFR_H);
    uint16_t r1 = d_rd16(1), r2 = d_rd16(2), r3 = d_rd16(3), r15 = get_r15();
    bool ok = r1 == t->e1 && r2 == t->e2 && r3 == t->e3 && r15 == t->e15;
    printf("      stop 1: R1=%04X R2=%04X R3=%04X R15=%04X (expect %04X %04X %04X %04X) CBR=%02X%02X\r\n",
           r1, r2, r3, r15, t->e1, t->e2, t->e3, t->e15, reg_read(GSU_CBR_H), reg_read(GSU_CBR_L));
    if (ok && t->has2) {
        set_r15(get_r15());
        t0 = time_us_64();
        while ((reg_read(GSU_SFR_L) & SFR_GO) && time_us_64() - t0 < 100000) {}
        (void)reg_read(GSU_SFR_H);
        r1 = d_rd16(1); r2 = d_rd16(2); r15 = get_r15();
        ok = r1 == t->e1b && r2 == t->e2b && r15 == t->e15b;
        printf("      stop 2: R1=%04X R2=%04X R15=%04X (expect %04X %04X %04X)\r\n",
               r1, r2, r15, t->e1b, t->e2b, t->e15b);
    }
    d_check(t->name, ok);
    return ok;
}

static void diag_lines(void) {
    printf("[6] cache line tests\r\n");
    static const struct lt tests[] = {
        {"program in line 1 only (start $10)", 0x10,
         {0xA1,0x5A,0x00,0x01,0xA2,0x77,0x00,0x01,1,1,1,1,1,1,1,1}, 1,
         0x10, 0x5A, 0, 0x14, 0x5A, 0x77, 0x18, true, 0},
        {"STOP at start of line 1, resume mid-line", 0x10,
         {0x00,0x01,0xA1,0x5A,0x00,0x01,1,1,1,1,1,1,1,1,1,1}, 1,
         0x10, 0, 0, 0x12, 0x5A, 0, 0x16, true, 0},
        {"fall through from line 0 into line 1", 0x00,
         {1,1,1,1,1,1,1,1,1,1,0xA1,0x5A,1,1,1,1,
          0xA2,0x77,0x00,0x01,1,1,1,1,1,1,1,1,1,1,1,1}, 2,
         0x0A, 0x5A, 0x77, 0x14, 0, 0, 0, false, 0},
        {"start $00, cross into line 1 (A1 01 .. A2 77)", 0x00,
         {0xA1,0x01,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
          0xA2,0x77,0x00,0x01,1,1,1,1,1,1,1,1,1,1,1,1}, 2,
         0x00, 0x01, 0x77, 0x14, 0, 0, 0, false, 0},
        {"start $02 inside line 0", 0x00,
         {1,1,0xA1,0x5A,0x00,0x01,1,1,1,1,1,1,1,1,1,1}, 1,
         0x02, 0x5A, 0, 0x06, 0, 0, 0, false, 0},
        {"start $10, first byte NOP (ROM has 00 there)", 0x10,
         {0x01,0xA1,0x5A,0x00,0x01,1,1,1,1,1,1,1,1,1,1,1}, 1,
         0x10, 0x5A, 0, 0x15, 0, 0, 0, false, 0},
        // Branch tests: both blocks written, start in block 0 (which works).
        // BRA offsets count from the byte after the operand; the byte after a
        // branch (NOP) executes first.
        {"BRA block 0 -> block 1 (forward)", 0x00,
         {0xA1,0x01, 0x05,0x0C, 0x01,1,1,1,1,1,1,1,1,1,1,1,
          0xA2,0x77, 0x00,0x01,1,1,1,1,1,1,1,1,1,1,1,1}, 2,
         0x00, 0x01, 0x77, 0x14, 0, 0, 0, false, 0},
        {"BRA block 0 -> 1 -> back to block 0", 0x00,
         {0xA1,0x01, 0x05,0x0C, 0x01,1,1,1, 0xA3,0x99,0x00,0x01,1,1,1,1,
          0xA2,0x77, 0x05,0xF4, 0x01,1,1,1,1,1,1,1,1,1,1,1}, 2,
         0x00, 0x01, 0x77, 0x0C, 0, 0, 0, false, 0xFF99},
        {"BRA inside block 1 (entered by BRA from block 0)", 0x00,
         {0xA1,0x01, 0x05,0x0C, 0x01,1,1,1,1,1,1,1,1,1,1,1,
          0xA2,0x77, 0x05,0x02, 0x01,1, 0xA3,0x99,0x00,0x01,1,1,1,1,1,1}, 2,
         0x00, 0x01, 0x77, 0x1A, 0, 0, 0, false, 0xFF99},
        {"program in line 31 only (start $1F0)", 0x1F0,
         {0xA1,0x5A,0x00,0x01,0xA2,0x77,0x00,0x01,1,1,1,1,1,1,1,1}, 1,
         0x1F0, 0x5A, 0, 0x1F4, 0x5A, 0x77, 0x1F8, true, 0},
    };
    for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; i++) diag_line_test(&tests[i]);
}

// [7] "last block" hypothesis for the MC1: host-written code is used only while
// execution stays in the block that ran last; entering another block reads ROM.
// Prediction: a block-0 program started right after a run that ended in block 1
// FAILS, and started again right after that (the failed run ended in block 0) PASSES.
static void diag_lastblock(void) {
    printf("[7] last-block test (MC1-like chips: step 2 FAIL, step 3 PASS; GSU-1: both PASS)\r\n");
    static const struct lt setup = {"step 1: run ending in block 1 (setup, result ignored)", 0x10,
        {0x00,0x01,0xA1,0x5A,0x00,0x01,1,1,1,1,1,1,1,1,1,1}, 1,
        0x10, 0, 0, 0x12, 0x5A, 0, 0x16, true, 0};
    static const struct lt blk0 = {"block-0 program, start $00", 0x00,
        {0xA1,0x5A,0x00,0x01,1,1,1,1,1,1,1,1,1,1,1,1}, 1,
        0x00, 0x5A, 0, 0x04, 0, 0, 0, false, 0};
    int f0 = diag_fail;
    (void)diag_line_test(&setup);
    diag_fail = f0;
    printf("    step 2 (after a run in block 1):\r\n");
    bool a = diag_line_test(&blk0);
    diag_fail = f0;
    printf("    step 3 (after a run in block 0):\r\n");
    bool b = diag_line_test(&blk0);
    diag_fail = f0;
    if (!a && b)      printf("    result: last-block-only behaviour (MC1-like).\r\n");
    else if (a && b)  printf("    result: all written blocks are kept (GSU-1-like).\r\n");
    else              printf("    result: unclear.\r\n");
}

// [8] cart RAM window (A10-A14 on GP21-GP25). Confirmed on a GSU-1 and a GSU-2.
// 8.1 float scan (informational): reads a few addresses in every 1 KB block with
//     the data lines pre-driven to 00 and to FF before the read. Values that follow
//     the pre-driven level mean a floating bus. A stable 00/00 is NOT proof that
//     the cart answers: a bus pulled low looks the same (seen on the MC1).
// 8.2 GSU write: a GSU program (STB from cache line 0) stores a byte into RAM; the
//     window must show it. This is the real use (the monitor reads what the GSU left).
// 8.3 host write: writes a different byte into each 1 KB block at two offsets and
//     reads them back (blocks alias when an A10-A12 line is stuck or swapped).
// 8.4 host write, GSU read: the host stores a byte through the window, the GSU loads
//     it (LDB) and the host reads it from R0. Tells whether window writes reach RAM
//     even when window reads do not.
// 8.5 SCMR variants: reads $0010 (where 8.2 left 5A) with SCMR = 00/08/10/18, in case
//     the S-CPU needs a different ownership setting.
static uint8_t diag_ram_peek_s(uint16_t off, uint8_t pre, uint8_t scmr) {
    reg_write(GSU_SCMR, scmr);
    gpio_put_masked(A_MASK | AH_MASK, ram_addr_bits(off));
    gpio_put_masked(D_MASK, (uint32_t)pre << PIN_D0);
    gpio_set_dir_out_masked(D_MASK);
    bus_delay();
    gpio_set_dir_in_masked(D_MASK);
    bus_delay();
    gpio_put(PIN_RD, 0);
    bus_delay();
    uint8_t v = (uint8_t)((gpio_get_all() & D_MASK) >> PIN_D0);
    gpio_put(PIN_RD, 1);
    bus_delay();
    gpio_put_masked(A_MASK | AH_MASK, (uint32_t)AH_REGS << PIN_A10);
    reg_write(GSU_SCMR, SCMR_NORMAL);
    return v;
}
static uint8_t diag_ram_peek(uint16_t off, uint8_t pre) { return diag_ram_peek_s(off, pre, SCMR_RON); }

// Runs "IBT R0,#val; IWT R3,#addr; STB (R3); NOP; NOP; STOP" from cache line 0.
static bool diag_gsu_store(uint16_t addr, uint8_t val) {
    uint8_t p[16] = {0xA0, val, 0xF3, (uint8_t)addr, (uint8_t)(addr >> 8), 0x3D, 0x33,
                     0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
    reg_write(GSU_CFGR, CFGR_IRQOFF);
    reg_write(GSU_CLSR, DIAG_CLSR);
    reg_write(GSU_SCMR, SCMR_NORMAL);
    reg_write(GSU_SFR_L, 0);
    reg_write(GSU_SFR_H, 0);
    for (uint8_t i = 0; i < 16; i++) reg_write(GSU_CACHE + i, p[i]);
    uint64_t t0 = time_us_64();
    set_r15(0);
    while ((reg_read(GSU_SFR_L) & SFR_GO) && time_us_64() - t0 < 100000) {}
    (void)reg_read(GSU_SFR_H);
    return !(reg_read(GSU_SFR_L) & SFR_GO);
}

static void diag_ramwin(void) {
    printf("[8] cart RAM window ($6000-$7FFF, A10-A14 = GP21-GP25)\r\n");
    // 8.1 float scan
    static const uint16_t offs[4] = {0x000, 0x155, 0x2AA, 0x3FF};
    int floating = 0;
    printf("    8.1 reads per block at offsets 000 155 2AA 3FF; \"a/b\" = bus pre-driven 00 / FF, '*' = differs\r\n");
    for (uint16_t k = 0; k < 8; k++) {
        printf("    block %u:", (unsigned)k);
        for (int o = 0; o < 4; o++) {
            uint16_t a = (uint16_t)((k << 10) | offs[o]);
            uint8_t x = diag_ram_peek(a, 0x00), y = diag_ram_peek(a, 0xFF);
            printf("  %02X/%02X%s", x, y, x != y ? "*" : " ");
            if (x != y) floating++;
        }
        printf("\r\n");
    }
    printf("    result: %s\r\n", floating ? "the bus follows the pre-driven level somewhere (floating)"
                                         : "stable values (cart answers, or the bus is pulled low)");

    // 8.2 GSU writes, window reads
    int gbad = 0;
    static const uint16_t ga[2] = {0x0010, 0x0410};
    for (int i = 0; i < 2; i++) {
        uint8_t before = diag_ram_peek(ga[i], 0x00);
        uint8_t val = (before == 0x5A) ? 0xA5 : 0x5A;
        bool done = diag_gsu_store(ga[i], val);
        uint8_t v;
        (void)ram_read(ga[i], &v, 1);
        printf("    8.2 GSU stored %02X at RAM $%04X (was %02X): window reads %02X%s\r\n", val,
               (unsigned)ga[i], before, v, done ? "" : " (GSU did not finish)");
        if (!done || v != val) gbad++;
    }
    d_check("8.2 byte stored by the GSU is visible through the window", gbad == 0);

    // 8.3 host writes, window reads
    int bad = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint16_t lo = pass ? 0x2AA : 0x155;
        for (uint16_t k = 0; k < 8; k++) ram_write((uint16_t)((k << 10) | lo), (uint8_t)(0x10 * k + 0x0F + pass));
        for (uint16_t k = 0; k < 8; k++) {
            uint8_t v, want = (uint8_t)(0x10 * k + 0x0F + pass);
            (void)ram_read((uint16_t)((k << 10) | lo), &v, 1);
            if (v != want) {
                printf("    block %u offset %03X wrote %02X read %02X\r\n", (unsigned)k, lo, want, v);
                bad++;
            }
        }
    }
    d_check("8.3 8 blocks x 2 offsets: host write, read back", bad == 0);

    // 8.4 host write through the window, GSU read
    {
        static const uint8_t pr[16] = {0xF3, 0x20, 0x00, 0x3D, 0x43, 0x01, 0x01, 0x00,
                                       0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
        int rbad = 0;
        for (int i = 0; i < 2; i++) {
            uint8_t val = i ? 0xC3 : 0x3C;
            ram_write(0x0020, val);
            reg_write(GSU_CFGR, CFGR_IRQOFF);
            reg_write(GSU_CLSR, DIAG_CLSR);
            reg_write(GSU_SCMR, SCMR_NORMAL);
            reg_write(GSU_SFR_L, 0);
            reg_write(GSU_SFR_H, 0);
            for (uint8_t k = 0; k < 16; k++) reg_write(GSU_CACHE + k, pr[k]);
            gsu_reg16_write(0, 0x0000);
            uint64_t t0 = time_us_64();
            set_r15(0);
            while ((reg_read(GSU_SFR_L) & SFR_GO) && time_us_64() - t0 < 100000) {}
            (void)reg_read(GSU_SFR_H);
            uint16_t r0 = d_rd16(0);
            printf("    8.4 host wrote %02X at RAM $0020 through the window: GSU LDB gives R0=%04X\r\n", val, r0);
            if (r0 != val) rbad++;
        }
        d_check("8.4 byte written through the window is seen by the GSU", rbad == 0);
    }

    // 8.5 SCMR variants
    printf("    8.5 read RAM $0010 (GSU stored 5A) with SCMR =");
    for (int i = 0; i < 4; i++) {
        uint8_t m = (uint8_t)(i * 8);
        printf("  %02X:%02X", m, diag_ram_peek_s(0x0010, 0x00, m));
    }
    printf("\r\n");
    uint8_t vcr = reg_read(GSU_VCR);
    d_check("registers still reachable afterwards", vcr != 0x00 && vcr != 0xFF);
}

static void diag_run(void) {
    diag_fail = 0;
    printf("\r\n==== GSU DIAG ====\r\n[1] identity\r\n");
    uint8_t vcr = reg_read(GSU_VCR);
    printf("    VCR=%02X SFR=%02X%02X CBR=%02X%02X\r\n", vcr,
           reg_read(GSU_SFR_H), reg_read(GSU_SFR_L),
           reg_read(GSU_CBR_H), reg_read(GSU_CBR_L));
    d_check("VCR is not 00/FF (cart answers)", vcr != 0x00 && vcr != 0xFF);

    reg_write(GSU_CFGR, CFGR_IRQOFF);
    reg_write(GSU_CLSR, DIAG_CLSR);
    reg_write(GSU_SCMR, SCMR_NORMAL);
    reg_write(GSU_PBR, 0);

    diag_regs();
    diag_cache();
    bool a = diag_start("normal config", CFGR_IRQOFF, DIAG_CLSR);
    bool b = diag_start("slow clock, IRQ unmasked", 0x00, 0x00);
    diag_clock();
    diag_lines();
    diag_lastblock();
    diag_ramwin();

    printf("==== %d check(s) failed ====\r\n", diag_fail);
    if (diag_fail == 0) {
        printf("All good: bus, registers, cache, start and resume work.\r\n");
    } else {
        if (!a && b) printf("Hint: works only at CLSR=0 -> clock/CFGR problem.\r\n");
        printf("Hints: register tests fail -> bus/level shifter/data lines;\r\n"
               "       registers ok but start fails -> GO not set, check A0-A9,\r\n"
               "       /WR timing (BUS_DELAY_CYCLES), cart clock, A10-A14 levels.\r\n");
    }
    printf("Press any key to run again.\r\n");
}
#endif

// -------------------------------------------------------------- monitor ----
// Entered by the exit syscall (op 1), with the GSU stopped and R15 = STOP + 2.
// Text over the same USB serial line. Commands (numbers are hex):
//   d               dump 256 bytes after the last dump (first time: from 0000)
//   d saddr         dump 256 bytes from saddr
//   d saddr,eaddr   dump saddr <= a <= eaddr (inclusive)
//                   cart RAM is 0000-1FFF through the $6000 window (8 KB), or 0000-7FFF through
//                   bank $70 (see b); an eaddr beyond it is cut off
//   b [0|1]         RAM read path for d: b = auto (bank $70 on the MC1, the window on the
//                   others), b 0 = $6000 window, b 1 = bank $70 (needs GP26, GP27 wired)
//   r               R0-R15, SFR, PBR, CBR
//   g               resume the GSU after the exit (not possible before the first load)
//   t               like g, and at the next exit print the elapsed time
//   l               load: boot the loader (prints READY, then send a program)
//   c 0|1           clock 10.7 MHz (default) / 21.4 MHz, applied at "l" or "g"
//   m 0|1           multiplier standard (default) / high speed (MS0), at "l" or "g";
//                   c 1 and m 1 exclude each other
//   i               print the banner line again (VCR, SFR, CLK, MODE, MS0)
//   ?               help
// The loader build starts in this monitor after reset; "l" loads the loader.
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parses a hex number after optional spaces; returns the next position, or
// NULL when there is no number.
static const char *mon_hex(const char *p, uint32_t *v) {
    while (*p == ' ') p++;
    if (hexval(*p) < 0) return NULL;
    uint32_t x = 0;
    while (hexval(*p) >= 0) x = (x << 4) | (uint32_t)hexval(*p++);
    *v = x;
    return p;
}

static void mon_getline(char *buf, int max) {
    int n = 0;
    for (;;) {
        int c = rx_get();
        if (c == '\r' || c == '\n') { printf("\r\n"); break; }
        if ((c == 0x08 || c == 0x7F) && n > 0) { n--; printf("\b \b"); fflush(stdout); continue; }
        if (c >= ' ' && c < 0x7F && n < max - 1) { buf[n++] = (char)c; putchar(c); fflush(stdout); }
    }
    buf[n] = 0;
}

static void mon_regs(void) {
    for (uint8_t n = 0; n < 16; n++)
        printf("R%-2u=%04X%s", n, gsu_reg16_read(n), (n % 4 == 3) ? "\r\n" : "  ");
    printf("SFR=%02X%02X", reg_read(GSU_SFR_H), reg_read(GSU_SFR_L));
    if (is_mc1()) printf(" PBR=n/a CBR=n/a (not readable on the MC1)\r\n");
    else printf(" PBR=%02X CBR=%02X%02X\r\n", reg_read(GSU_PBR), reg_read(GSU_CBR_H), reg_read(GSU_CBR_L));
}

static uint16_t mon_next;                    // where a bare "d" continues
static bool opt_bank_auto = true;            // b: auto (bank $70 on the MC1, window elsewhere)
static bool opt_bank = false;                //    or forced: b 0 window, b 1 bank $70
static bool dump_bank(void) { return opt_bank_auto ? is_mc1() : opt_bank; }

// Dumps start..end inclusive (end already clamped to the window). Rows are
// aligned to 16; bytes outside start..end are left blank.
static void mon_dump(bool bank, uint16_t start, uint16_t end) {
    uint8_t buf[16];
    const uint16_t size = bank ? RAM_BANK_SIZE : RAM_WINDOW_SIZE;
    for (uint16_t row = start & ~15u; row <= end; row += 16) {
        uint16_t lo = row < start ? start : row;
        uint16_t hi = row + 15 > end ? end : (uint16_t)(row + 15);
        uint16_t n = ram_read_ex(bank, lo, buf, (uint16_t)(hi - lo + 1));
        printf("%04X:", row);
        for (uint16_t a = row; a < row + 16; a++)
            if (a >= lo && a < lo + n) printf(" %02X", buf[a - lo]); else printf("   ");
        printf("  ");
        for (uint16_t a = row; a < row + 16; a++) {
            if (a >= lo && a < lo + n) putchar(buf[a - lo] >= ' ' && buf[a - lo] < 0x7F ? buf[a - lo] : '.');
            else putchar(' ');
        }
        printf("\r\n");
    }
    mon_next = end + 1 >= size ? 0 : end + 1;
}

static bool is_mc1(void) { return reg_read(GSU_VCR) == 0x01; }   // MARIO chip: VCR=01

// "c 0|1" and "m 0|1": p points after the command letter. Takes effect at "l".
static void mon_cmd_cm(char cmd, const char *p) {
    uint32_t v;
    if (!mon_hex(p, &v) || v > 1) { printf("usage: %c 0|1\r\n", cmd); return; }
    if (cmd == 'c') {
        if (v && opt_ms0) { printf("c 1 refused: m 1 is set (MS0 is for 10.7 MHz only); use m 0 first\r\n"); return; }
        opt_clk21 = v;
        if (v && is_mc1()) printf("warning: the MC1 runs away at 21.4 MHz in longer programs\r\n");
    } else {
        if (v && opt_clk21) { printf("m 1 refused: the clock is 21.4 MHz (MS0 is for 10.7 MHz only); use c 0 first\r\n"); return; }
        opt_ms0 = v;
    }
    printf("clock %s, multiplier %s (applied at l or g)\r\n", opt_clk21 ? "21.4 MHz" : "10.7 MHz",
           opt_ms0 ? "high speed (MS0=1)" : "standard");
}

// "d", "d saddr" or "d saddr,eaddr" (p points after the 'd').
static void mon_cmd_d(const char *p) {
    const bool bank = dump_bank();
    const uint16_t size = bank ? RAM_BANK_SIZE : RAM_WINDOW_SIZE;
    uint32_t s = mon_next, e;
    const char *q = mon_hex(p, &s);
    if (q) {
        while (*q == ' ') q++;
        if (*q == ',') {
            q = mon_hex(q + 1, &e);
            if (!q) { printf("usage: d [saddr[,eaddr]]\r\n"); return; }
        } else {
            e = s + 255;
        }
    } else {
        e = s + 255;
    }
    if (s >= size) { printf("address must be 0000-%04X\r\n", size - 1); return; }
    if (e >= size) e = size - 1u;
    if (e < s) { printf("eaddr is below saddr\r\n"); return; }
    if (mon_next >= size) mon_next = 0;
    mon_dump(bank, (uint16_t)s, (uint16_t)e);
}

// "b", "b 0", "b 1": how d reads the RAM. p points after the 'b'.
static void mon_cmd_b(const char *p) {
    uint32_t v;
    if (mon_hex(p, &v)) {
        if (v > 1) { printf("usage: b [0|1]\r\n"); return; }
        opt_bank_auto = false; opt_bank = v;
    } else {
        opt_bank_auto = true;
    }
    mon_next = 0;
    printf("d reads RAM through %s%s\r\n", dump_bank() ? "bank $70 (32 KB, GP26/GP27)" : "the $6000 window (8 KB)",
           opt_bank_auto ? " (auto)" : "");
}

static void print_banner(void);

// Returns what to do next: load the loader, resume (g), or resume and time the run (t).
// `resumable` is false right after reset, when no program has run yet.
enum mon_result { MON_LOAD, MON_GO, MON_TIMED };

static enum mon_result monitor(bool resumable) {
    char line[40];
    printf("cmd: d dump, r regs, g go, t timed go, l load, c clock, m mul, i info, ? help\r\n");
    for (;;) {
        printf("mon> "); fflush(stdout);
        mon_getline(line, sizeof line);
        const char *p = line;
        while (*p == ' ') p++;
        char cmd = *p ? *p++ : 0;
        switch (cmd) {
        case 0: break;
        case 'd': mon_cmd_d(p); break;
        case 'b': mon_cmd_b(p); break;
        case 'r': mon_regs(); break;
        case 'g':
            if (resumable) return MON_GO;
            printf("nothing to resume: use l to load a program\r\n");
            break;
        case 't':
            if (resumable) return MON_TIMED;
            printf("nothing to resume: use l to load a program\r\n");
            break;
        case 'l': return MON_LOAD;
        case 'c': case 'm': mon_cmd_cm(cmd, p); break;
        case 'i': print_banner(); break;
        default:
            printf("d [saddr[,eaddr]]  dump RAM (0000-1FFF window, 0000-7FFF bank $70)\r\n"
                   "b [0|1]            RAM read path: auto, $6000 window, bank $70\r\n"
                   "r                  registers\r\n"
                   "g                  go (resume after exit)\r\n"
                   "t                  timed go\r\n"
                   "l                  load a program\r\n"
                   "c 0|1              clock 10.7 / 21.4 MHz\r\n"
                   "m 0|1              multiplier standard / high speed (MS0)\r\n"
                   "i                  info (banner)\r\n"
                   "?                  this help\r\n");
        }
    }
}

// ---------------------------------------------------------------- boot ----
// Writes CFGR and CLSR from the c/m settings. With force=false (monitor "g") it
// writes only what changed since the last write, so an unchanged "g" touches
// nothing. The GSU is stopped (GO=0) whenever this is called. [VERIFY]: the program keeps
// running after the change (GSU-1), but the effect on the clock is not measured.
static void gsu_apply_config(bool force) {
    if (force || !applied_valid || opt_ms0 != applied_ms0)
        reg_write(GSU_CFGR, CFGR_IRQOFF | (opt_ms0 ? CFGR_MS0 : 0));
    if (force || !applied_valid || opt_clk21 != applied_clk21)
        reg_write(GSU_CLSR, opt_clk21 ? 0x01 : 0x00);
    applied_valid = true; applied_clk21 = opt_clk21; applied_ms0 = opt_ms0;
}

// Configure the GSU, load the cache program and start it; also used by the
// monitor's "l". With the loader build this is stage 1: the firmware presets
// R2 = byte count, R1 = 0 (RAM address), R4 = 0 (jump target), R8 = $70 (RAM
// bank) and serves the first sizeof loader_prog read syscalls from loader_prog.
// restart=true (loader build, after a program has run on the MC1): the MC1 keeps
// the cache window (CBR) and the cache lines of the program that ran, ignores host
// writes to them, and so cannot run stage 1 again (an experiment showed the first
// instruction fetched at $0000 was 00 = STOP). Instead the loader that stage 1
// left in cart RAM at $0000 is started directly with PBR=$70 (see "RAM loader" in
// the README: programs must not write $0000-$004F).
static void gsu_boot(bool restart) {
    gsu_apply_config(true);
    reg_write(GSU_SCMR, SCMR_NORMAL);
#if BOOT_ACTIVE
    if (restart) {
        reg_write(GSU_PBR, 0x70);
        gsu_reg16_write(8, 0x0070);          // RAM bank for the loader's LJMP
        boot_left = 0;                       // the loader in RAM is already complete
        set_r15(0x0000);
        printf("READY\r\n");
        return;
    }
#endif
    reg_write(GSU_PBR,  0x00);

    load_program();
#if BOOT_ACTIVE
    gsu_reg16_write(2, sizeof loader_prog);  // byte count
    gsu_reg16_write(1, 0x0000);              // RAM address
    gsu_reg16_write(4, 0x0000);              // jump target after loading
    gsu_reg16_write(8, 0x0070);              // RAM bank
    boot_src = loader_prog;
    boot_left = sizeof loader_prog;
#endif
    set_r15(0x0000);
    printf("READY\r\n");                     // binary data follows from here
}

// ---------------------------------------------------------------- main ----
// CLK is what the host will set in CLSR (CLSR cannot be read back).
static void print_banner(void) {
    printf("GSU host v%s: VCR=%02X SFR=%02X%02X CLK=%s MODE=%s%s\r\n", GSU_HOST_VERSION,
           reg_read(GSU_VCR), reg_read(GSU_SFR_H), reg_read(GSU_SFR_L),
           opt_clk21 ? "21.4MHz" : "10.7MHz",
           GSU_DIAG ? "diag" : GSU_MEASURE ? "measure" : "loader",
           opt_ms0 ? " MS0=1" : "");
}

int main(void) {
    stdio_init_all();

    sleep_ms(2000);                          // let USB serial enumerate
    bus_init();
    cart_reset();

    printf("\r\n");
    print_banner();

#if GSU_DIAG
    for (;;) {
        diag_run();
        (void)getchar();
    }
#endif
#if GSU_MEASURE
    measure_bus();
#endif
#if BOOT_ACTIVE
    (void)monitor(false);                    // loader build: wait for "l" (only "l" returns)
#endif
    gsu_boot(false);
#if GSU_MEASURE
    uint64_t t_start = time_us_64();
    uint32_t meas_count = 0;
#endif

    // "t" timing: wall time from the resume to the next exit, the time the GSU
    // itself ran (resume to STOP, summed over the syscalls), and the syscall count.
    bool timing = false;
    uint64_t t_begin = 0, t_resume = time_us_64(), gsu_us = 0;
    uint32_t sys_count = 0;

    for (;;) {
        // Wait for STOP (GO cleared). Reading SFR also clears the IRQ flag.
#if GSU_WATCHDOG_MS
        uint64_t t0 = time_us_64();
#endif
        while (gsu_running()) {
#if GSU_WATCHDOG_MS
            if (time_us_64() - t0 > (uint64_t)GSU_WATCHDOG_MS * 1000) {
                printf("\r\nTIMEOUT: GSU still running, SFR=%02X%02X\r\n",
                       reg_read(GSU_SFR_H), reg_read(GSU_SFR_L));
                t0 = time_us_64();
            }
#endif
        }
        uint64_t t_stop = time_us_64();
        (void)reg_read(GSU_SFR_H);

        uint8_t op = reg_read(GSU_R(3));
        if (timing && op != SYS_EXIT) { gsu_us += t_stop - t_resume; sys_count++; }
        switch (op) {
        case SYS_READ:                       // USB -> R0 (zero-extended)
#if BOOT_ACTIVE
            if (boot_left) { boot_left--; gsu_reg16_write(0, *boot_src++); break; }
#endif
            gsu_reg16_write(0, rx_get());
            break;
        case SYS_WRITE:                      // R0 low -> USB
            putchar(reg_read(GSU_R(0)));
            fflush(stdout);
            break;
        case SYS_FSTAT:                      // status -> R0 (zero-extended)
            gsu_reg16_write(0, sys_status());
            break;
        case SYS_NOP:                        // nothing to do
            break;
        case SYS_EXIT: {                     // hand control to the monitor
            printf("\r\nEXIT R15=%04X\r\n", get_r15());
            if (timing) {
                gsu_us += t_stop - t_resume;
                uint64_t total = t_stop - t_begin;
                printf("time %lu.%03lu ms (GSU run %lu.%03lu ms, %lu syscalls)\r\n",
                       (unsigned long)(total / 1000), (unsigned long)(total % 1000),
                       (unsigned long)(gsu_us / 1000), (unsigned long)(gsu_us % 1000),
                       (unsigned long)sys_count);
                timing = false;
            }
            enum mon_result r = monitor(true);
            if (r == MON_LOAD) { gsu_boot(is_mc1()); t_resume = time_us_64(); continue; }
            gsu_apply_config(false);         // g, t: c/m changes take effect now
            if (r == MON_TIMED) {
                timing = true; gsu_us = 0; sys_count = 0;
                t_begin = time_us_64();
            }
            break;                           // resume like any other syscall
        }
        default:
            printf("\r\nBAD OP %02X R15=%04X\r\n", op, get_r15());
            timing = false;
            (void)monitor(false);            // r shows the registers; only "l" returns
            gsu_boot(is_mc1());
            t_resume = time_us_64();
            continue;
        }

        // R15 already holds STOP+2, so writing it back resumes correctly.
        set_r15(get_r15());                  // high-byte write sets GO again
        t_resume = time_us_64();
#if GSU_MEASURE
        if (++meas_count == MEASURE_COUNT) {
            report_measure(time_us_64() - t_start);
            for (;;) tight_loop_contents();  // GSU keeps looping; host is done
        }
#endif
    }
}

// gsu_host.c — RP2040 (Pico 1) host for a real Super FX (GSU) cartridge.
//
// The Pico plays the role of the S-CPU: it only touches the GSU register
// window ($3000-$32FF) and never touches cart ROM/RAM. It loads a small GSU
// program into the instruction cache, starts it, and treats STOP as a
// system call into USB serial.
//
// ABI (GSU side): R1 low byte = op, R0 low byte = data, then STOP + dummy byte.
//   op 0  read    blocks until a byte arrives; R0 <- byte (zero-extended)
//   op 1  write   R0 low -> USB serial
//   op 2  status  R0 <- 6850-ACIA-style status (zero-extended), bit0 = RDRF
//                 (receive data available), bit1 = TDRE (can transmit)
//
// STOP semantics (fullsnes, GSU-n chapter): STOP clears GO, sets IRQ (even if
// masked) and leaves R15 = STOP address + 2. The byte after STOP is
// prefetched but NOT executed, so it is a dummy and resume skips it.
// The code cache is NOT cleared by STOP; it is cleared by CACHE, LJMP, or a
// host write of GO=0 to SFR (which also sets CBR=0).
//
// STATUS: start, STOP and resume confirmed on a GSU-1 with -DGSU_DIAG=1.
// Items marked [VERIFY] are not yet run on hardware.

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

// A10..A22 must be hard-wired so the cart decodes $3000-$32FF in bank $00
// (A23 and /CART are not connected on the inspected cart PCB; leave open).

// Not needed on the inspected cart (board 1CA6B-01): its PCB has no connection
// from the console clock (pin 1) or PHI2 (pin 57), and the oscillator is on the
// cart.
// Kept, default off, for carts that take the clock from the connector: drives
// 21.477 MHz on GP21 (clk_gpout0); the fractional divider gives a jittery clock.
#define ENABLE_GSU_CLK_OUT 0
#define PIN_CLK            21

// Strobe/setup delay in system-clock cycles (64 = ~0.5 us at 125 MHz).
// Increase if reads are flaky.
#define BUS_DELAY_CYCLES   64

// TDRE uses the TinyUSB CDC write FIFO. Set to 0 to report TDRE = "USB
// connected" using only the stdio API.
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
#define CFGR_IRQOFF 0x80                 // bit7 = IRQ mask (we poll GO instead).
                                         // NOT 0x20: that is MS0 (multiplier high
                                         // speed), which must be 0 with CLSR=1.
#define CLSR_21MHZ  0x01

// System call numbers (R1 low byte)
#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_STATUS  2
#define SYS_NOP     3                    // does nothing; used by the MEASURE build

// STOP does not flush the cache, so rewriting the program on every resume is
// unnecessary. Set to 1 only for debugging. Never write SFR with GO=0 while
// relying on the cache: that clears it.
#define RELOAD_CACHE_ON_RESUME 0

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
#ifndef MEASURE_COUNT
#define MEASURE_COUNT 1000000u
#endif

// ----------------------------------------------------------- bus layer ----
static inline void bus_delay(void) { busy_wait_at_least_cycles(BUS_DELAY_CYCLES); }

static void bus_init(void) {
    const uint32_t ctl = (1u << PIN_RD) | (1u << PIN_WR) | (1u << PIN_RESET);
    gpio_init_mask(A_MASK | D_MASK | ctl);
    gpio_put_masked(A_MASK | ctl, ctl);          // A=0, strobes inactive (high)
    gpio_set_dir_out_masked(A_MASK | ctl);
    gpio_set_dir_in_masked(D_MASK);
}

static void reg_write(uint16_t reg, uint8_t v) {
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

// ------------------------------------------------------- GSU program ------
// Two 16-byte cache lines, hand-assembled. [VERIFY] opcodes and branch
// offsets against a GSU assembler such as bass or WLA-DX.
//
//  00: A1 02     IBT R1,#2        ; op = status
//  02: 00        STOP             ; syscall (R15 will be $04 on resume)
//  03: 01        (dummy byte, prefetched but not executed)
//  04: 3E 71     AND #1           ; R0 = R0 & 1 (RDRF), sets Z
//  06: 09 F8     BEQ $00          ; no data yet -> poll again (-8)
//  08: 01        NOP              ; branch delay slot
//  09: A1 00     IBT R1,#0        ; op = read
//  0B: 00        STOP
//  0C: 01        (dummy)
//  0D: D0        INC R0           ; R0 = R0 + 1
//  0E: A1 01     IBT R1,#1        ; op = write
//  10: 00        STOP
//  11: 01        (dummy)
//  12: 05 EC     BRA $00          ; -(0x14)
//  14: 01        NOP              ; branch delay slot
//  15-1F: 01     padding
//
// Behaviour: wait until a byte is available, read it, add 1, write it back.
static const uint8_t demo_prog[32] __attribute__((unused)) = {
    0xA1, 0x02, 0x00, 0x01, 0x3E, 0x71, 0x09, 0xF8,
    0x01, 0xA1, 0x00, 0x00, 0x01, 0xD0, 0xA1, 0x01,
    0x00, 0x01, 0x05, 0xEC, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
};

// RAM loader (default program). Protocol over USB serial, all little endian:
//   addr(2)  load address in cart RAM  (RAM bank 0 = $70)
//   entry(2) address to jump to afterwards
//   len(2)   number of payload bytes, must be >= 1
//   payload  len bytes
// The loader reads each byte with the read syscall, stores it with STB,
// then does LJMP to bank $70 at `entry`. LJMP empties the cache, which then
// refills from RAM. The loaded program keeps using the same syscalls.
//
// Each 16-bit read is a 16-byte macro (d = 3, 4, 2 for addr, entry, len):
//   A1 00        IBT R1,#0        ; op = read
//   00 01        STOP + dummy     ; R0 low = byte
//   1d B0 9E     TO Rd; FROM R0; LOB          ; Rd = low byte
//   A1 00 00 01  IBT R1,#0; STOP + dummy
//   9E 4D        LOB; SWAP        ; R0 = high byte << 8
//   1d B0 Cd     TO Rd; FROM R0; OR Rd        ; Rd |= R0
// then the copy loop:
//   30: A1 00 00 01   IBT R1,#0; STOP + dummy
//   34: 3D 33         STB (R3)    ; ALT1 + store byte
//   36: D3            INC R3
//   37: E2            DEC R2      ; sets Z
//   38: 08 F6 01      BNE $30; NOP (delay slot)
//   3B: A8 70         IBT R8,#$70
//   3D: B4 3D 98 01   FROM R4; ALT1; LJMP R8; NOP (delay slot)
// Checked only against a small simulator of my own, not against hardware or
// a GSU assembler. [VERIFY]
static const uint8_t loader_prog[80] __attribute__((unused)) = {
    // addr -> R3
    0xA1, 0x00, 0x00, 0x01, 0x13, 0xB0, 0x9E, 0xA1,
    0x00, 0x00, 0x01, 0x9E, 0x4D, 0x13, 0xB0, 0xC3,
    // entry -> R4
    0xA1, 0x00, 0x00, 0x01, 0x14, 0xB0, 0x9E, 0xA1,
    0x00, 0x00, 0x01, 0x9E, 0x4D, 0x14, 0xB0, 0xC4,
    // len -> R2
    0xA1, 0x00, 0x00, 0x01, 0x12, 0xB0, 0x9E, 0xA1,
    0x00, 0x00, 0x01, 0x9E, 0x4D, 0x12, 0xB0, 0xC2,
    // copy loop, then LJMP
    0xA1, 0x00, 0x00, 0x01, 0x3D, 0x33, 0xD3, 0xE2,
    0x08, 0xF6, 0x01, 0xA8, 0x70, 0xB4, 0x3D, 0x98,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
};

// Measurement program: calls the NOP syscall forever.
//  00: A1 03     IBT R1,#3        ; op = nop
//  02: 00        STOP
//  03: 01        (dummy)
//  04: 05 FA     BRA $00          ; -6 (base = byte after operand)
//  06: 01        NOP              ; branch delay slot
static const uint8_t measure_prog[16] __attribute__((unused)) = {
    0xA1, 0x03, 0x00, 0x01, 0x05, 0xFA, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
};

// Select the cache program: loader (default), the +1 echo demo
// (-DGSU_PROGRAM_DEMO=1), or the measurement loop (-DGSU_MEASURE=1,
// which takes precedence).
#ifndef GSU_PROGRAM_DEMO
#define GSU_PROGRAM_DEMO 0
#endif
#if GSU_MEASURE
#define gsu_prog measure_prog
#elif GSU_PROGRAM_DEMO
#define gsu_prog demo_prog
#else
#define gsu_prog loader_prog
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
    reg_write(GSU_SCMR, SCMR_RAN | SCMR_RON);
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

static void diag_run(void) {
    diag_fail = 0;
    printf("\r\n==== GSU DIAG ====\r\n[1] identity\r\n");
    uint8_t vcr = reg_read(GSU_VCR);
    printf("    VCR=%02X SFR=%02X%02X CBR=%02X%02X\r\n", vcr,
           reg_read(GSU_SFR_H), reg_read(GSU_SFR_L),
           reg_read(GSU_CBR_H), reg_read(GSU_CBR_L));
    d_check("VCR is not 00/FF (cart answers)", vcr != 0x00 && vcr != 0xFF);

    reg_write(GSU_CFGR, CFGR_IRQOFF);
    reg_write(GSU_CLSR, CLSR_21MHZ);
    reg_write(GSU_SCMR, SCMR_RAN | SCMR_RON);
    reg_write(GSU_PBR, 0);

    diag_regs();
    diag_cache();
    bool a = diag_start("normal config", CFGR_IRQOFF, CLSR_21MHZ);
    bool b = diag_start("slow clock, IRQ unmasked", 0x00, 0x00);

    printf("==== %d check(s) failed ====\r\n", diag_fail);
    if (diag_fail == 0) {
        printf("All good: bus, registers, cache, start and resume work.\r\n");
    } else {
        if (!a && b) printf("Hint: works only at CLSR=0 -> clock/CFGR problem.\r\n");
        printf("Hints: register tests fail -> bus/level shifter/data lines;\r\n"
               "       registers ok but start fails -> GO not set, check A0-A9,\r\n"
               "       /WR timing (BUS_DELAY_CYCLES), cart clock, fixed address levels.\r\n");
    }
    printf("Press any key to run again.\r\n");
}
#endif

// ---------------------------------------------------------------- main ----
int main(void) {
    stdio_init_all();

#if ENABLE_GSU_CLK_OUT
    clock_gpio_init(PIN_CLK, CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    (float)clock_get_hz(clk_sys) / 21477270.0f);
#endif

    sleep_ms(2000);                          // let USB serial enumerate
    bus_init();
    cart_reset();

    printf("\r\nGSU host: VCR=%02X SFR=%02X%02X\r\n",
           reg_read(GSU_VCR), reg_read(GSU_SFR_H), reg_read(GSU_SFR_L));

#if GSU_DIAG
    for (;;) {
        diag_run();
        (void)getchar();
    }
#endif
#if GSU_MEASURE
    measure_bus();
#endif
    reg_write(GSU_CFGR, CFGR_IRQOFF);
    reg_write(GSU_CLSR, CLSR_21MHZ);
    reg_write(GSU_SCMR, SCMR_RAN | SCMR_RON);
    reg_write(GSU_PBR,  0x00);

    load_program();
    set_r15(0x0000);
#if GSU_PROGRAM_DEMO
    printf("demo: type characters, each returns +1\r\n");
#endif
    printf("READY\r\n");                     // binary data follows from here
#if GSU_MEASURE
    uint64_t t_start = time_us_64();
    uint32_t meas_count = 0;
#endif

    for (;;) {
        // Wait for STOP (GO cleared). Reading SFR also clears the IRQ flag.
        uint64_t t0 = time_us_64();
        while (gsu_running()) {
            if (time_us_64() - t0 > 500000) {
                printf("\r\nTIMEOUT: GSU still running, SFR=%02X%02X\r\n",
                       reg_read(GSU_SFR_H), reg_read(GSU_SFR_L));
                t0 = time_us_64();
            }
        }
        (void)reg_read(GSU_SFR_H);

        uint8_t op = reg_read(GSU_R(1));
        switch (op) {
        case SYS_READ:                       // USB -> R0 (zero-extended)
            gsu_reg16_write(0, rx_get());
            break;
        case SYS_WRITE:                      // R0 low -> USB
            putchar(reg_read(GSU_R(0)));
            fflush(stdout);
            break;
        case SYS_STATUS:                     // status -> R0 (zero-extended)
            gsu_reg16_write(0, sys_status());
            break;
        case SYS_NOP:                        // nothing to do
            break;
        default:
            printf("\r\nBAD OP %02X R15=%04X\r\n", op, get_r15());
            for (;;) tight_loop_contents();
        }

        // R15 already holds STOP+2, so writing it back resumes correctly.
#if RELOAD_CACHE_ON_RESUME
        uint16_t pc = get_r15();
        load_program();
        set_r15(pc);
#else
        set_r15(get_r15());                  // high-byte write sets GO again
#endif
#if GSU_MEASURE
        if (++meas_count == MEASURE_COUNT) {
            report_measure(time_us_64() - t_start);
            for (;;) tight_loop_contents();  // GSU keeps looping; host is done
        }
#endif
    }
}

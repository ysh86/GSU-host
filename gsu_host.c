// gsu_host.c — RP2040 (Pico 1) host for a real Super FX (GSU) cartridge.
//
// The Pico plays the role of the S-CPU: it only touches the GSU register
// window ($3000-$32FF) and never touches cart ROM/RAM. It loads a small GSU
// program into the instruction cache, starts it, and treats STOP as a
// system call into USB serial.
//
// ABI (GSU side): R1 low byte = op, R0 low byte = data, then STOP + dummy byte.
//   op 0  read    blocks until a byte arrives; R0 low <- byte
//   op 1  write   R0 low -> USB serial
//   op 2  status  R0 low <- 6850-ACIA-style status, bit0 = RDRF
//                 (receive data available), bit1 = TDRE (can transmit)
//
// STOP semantics (fullsnes, GSU-n chapter): STOP clears GO, sets IRQ (even if
// masked) and leaves R15 = STOP address + 2. The byte after STOP is
// prefetched but NOT executed, so it is a dummy and resume skips it.
// The code cache is NOT cleared by STOP; it is cleared by CACHE, LJMP, or a
// host write of GO=0 to SFR (which also sets CBR=0).
//
// STATUS: written without hardware access. Items marked [VERIFY] are
// assumptions taken from public documentation.

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

// A10..A23 must be hard-wired so the cart decodes $3000-$32FF in bank $00.
// [VERIFY] against the cart's schematic / decode logic.

// Optional: drive a 21.477 MHz clock on GP21 (clk_gpout0) in case the GSU
// clock is not supplied on the cart itself. [VERIFY] whether needed; the
// fractional divider gives a jittery clock, so default off.
#define ENABLE_GSU_CLK_OUT 0
#define PIN_CLK            21

// Strobe/setup delay in system-clock cycles (64 = ~0.5 us at 125 MHz).
// Increase if reads are flaky. [VERIFY]
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
#define GSU_CACHE   0x3100               // 512-byte instruction cache

#define SFR_GO      0x20                 // bit 5 of SFR low byte
#define SCMR_RAN    0x08
#define SCMR_RON    0x10
#define CFGR_IRQOFF 0x20                 // mask IRQ (we poll GO instead)
#define CLSR_21MHZ  0x01

// System call numbers (R1 low byte)
#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_STATUS  2

// STOP does not flush the cache, so rewriting the program on every resume is
// unnecessary. Set to 1 only for debugging. Never write SFR with GO=0 while
// relying on the cache: that clears it.
#define RELOAD_CACHE_ON_RESUME 0

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

// Select the cache program: loader (default) or the +1 echo demo
// (build with -DGSU_PROGRAM_DEMO=1).
#ifndef GSU_PROGRAM_DEMO
#define GSU_PROGRAM_DEMO 0
#endif
#if GSU_PROGRAM_DEMO
#define gsu_prog demo_prog
#else
#define gsu_prog loader_prog
#endif

// A cache line becomes valid when its last byte ($xxxF) is written, so
// write in ascending order. [VERIFY]
static void load_program(void) {
    for (uint16_t i = 0; i < sizeof gsu_prog; i++)
        reg_write(GSU_CACHE + i, gsu_prog[i]);
}

static void set_r15(uint16_t pc) {
    reg_write(GSU_R(15),     pc & 0xFF);     // low byte first
    reg_write(GSU_R(15) + 1, pc >> 8);       // high byte write sets GO [VERIFY]
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

    for (;;) {
        // Wait for STOP (GO cleared). Reading SFR also clears the IRQ flag.
        uint64_t t0 = time_us_64();
        while (reg_read(GSU_SFR_L) & SFR_GO) {
            if (time_us_64() - t0 > 500000) {
                printf("\r\nTIMEOUT: GSU still running, SFR=%02X%02X\r\n",
                       reg_read(GSU_SFR_H), reg_read(GSU_SFR_L));
                t0 = time_us_64();
            }
        }
        (void)reg_read(GSU_SFR_H);

        uint8_t op = reg_read(GSU_R(1));
        switch (op) {
        case SYS_READ:                       // USB -> R0 low
            reg_write(GSU_R(0), rx_get());
            break;
        case SYS_WRITE:                      // R0 low -> USB
            putchar(reg_read(GSU_R(0)));
            fflush(stdout);
            break;
        case SYS_STATUS:                     // status -> R0 low
            reg_write(GSU_R(0), sys_status());
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
    }
}

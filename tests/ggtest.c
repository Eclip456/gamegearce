/*
 * Checks the Game Gear machine (mapper, VDP ports, frame interrupts) with a
 * small hand-assembled ROM. Exits non-zero on failure.
 */
#include <stdio.h>
#include <string.h>

#include "gg.h"
#include "vdp.h"
#include "z80.h"

#define PAGES 4

static uint8_t rom[PAGES][0x4000];
static int failures;

static void put(uint16_t addr, const uint8_t *code, size_t len)
{
    memcpy(&rom[0][addr], code, len);
}

static void expect(const char *what, unsigned got, unsigned want)
{
    printf("%-34s %s (got %02X, want %02X)\n", what, got == want ? "ok  " : "FAIL", got, want);
    if (got != want)
        failures++;
}

int main(void)
{
    static const uint8_t reset[] = {
        0xF3,                   /* DI */
        0xED, 0x56,             /* IM 1 */
        0x31, 0xF0, 0xDF,       /* LD SP,DFF0 */
        0xC3, 0x00, 0x01,       /* JP 0100 */
    };
    static const uint8_t isr[] = {
        0xF5,                   /* PUSH AF */
        0xDB, 0xBF,             /* IN A,(BF)    acknowledge */
        0x3A, 0x00, 0xC0,       /* LD A,(C000) */
        0x3C,                   /* INC A */
        0x32, 0x00, 0xC0,       /* LD (C000),A */
        0xF1,                   /* POP AF */
        0xFB,                   /* EI */
        0xED, 0x4D,             /* RETI */
    };
    static const uint8_t main_code[] = {
        0x3E, 0x20, 0xD3, 0xBF, /* VDP reg 1 = 20: frame interrupts on */
        0x3E, 0x81, 0xD3, 0xBF,
        0x3E, 0x00, 0xD3, 0xBF, /* CRAM address 0 */
        0x3E, 0xC0, 0xD3, 0xBF,
        0x3E, 0x34, 0xD3, 0xBE, /* color 0 = 0x0234 */
        0x3E, 0x12, 0xD3, 0xBE,
        0x3E, 0x03,             /* LD A,3 */
        0x32, 0xFF, 0xFF,       /* LD (FFFF),A   slot 2 = page 3 */
        0x3A, 0x00, 0x80,       /* LD A,(8000) */
        0x32, 0x01, 0xC0,       /* LD (C001),A */
        0x3E, 0x5A,             /* LD A,5A */
        0x32, 0x04, 0xE0,       /* LD (E004),A   write through the mirror */
        0x3A, 0x04, 0xC0,       /* LD A,(C004) */
        0x32, 0x02, 0xC0,       /* LD (C002),A */
        0xDB, 0x00,             /* IN A,(00)     Start button */
        0x32, 0x03, 0xC0,       /* LD (C003),A */
        0xFB,                   /* EI */
        0x76,                   /* loop: HALT */
        0x18, 0xFD,             /* JR loop */
    };
    const uint8_t *pages[PAGES];

    for (int p = 0; p < PAGES; p++)
    {
        rom[p][0] = 0xA0 + p;
        pages[p] = rom[p];
    }
    put(0x0000, reset, sizeof reset);
    put(0x0038, isr, sizeof isr);
    put(0x0100, main_code, sizeof main_code);

    gg_init(pages, PAGES);
    gg.buttons = GG_START;
    for (int i = 0; i < 10; i++)
        gg_run_frame();

    expect("frame interrupts in 10 frames", z80_rmap[0xC0].p[0], 10);
    expect("slot 2 read after bank switch", z80_rmap[0xC0].p[1], 0xA3);
    expect("RAM mirror at E000", z80_rmap[0xC0].p[2], 0x5A);
    expect("Start pressed reads bit 7 low", z80_rmap[0xC0].p[3], 0x40);
    expect("mapper slot 2 register", gg.bank[2], 3);
    expect("VDP register 1", vdp.reg[1], 0x20);
    expect("palette color 0 low byte", vdp.cram[0], 0x34);
    expect("palette color 0 high byte", vdp.cram[1], 0x02);
    expect("CPU halted in main loop", z80.halted, 1);

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures != 0;
}

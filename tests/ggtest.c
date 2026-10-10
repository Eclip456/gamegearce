/*
 * Checks the Game Gear machine (mapper, VDP ports, frame interrupts, code
 * running across bank and RAM boundaries) with a small hand-assembled ROM.
 *
 * PC build: the C core; prints the results and exits non-zero on failure.
 * Calculator build (tests/calc, GGMACH): the assembly core with its code
 * buffer; bench/cemu/run prints the report.
 */
#include <stdio.h>
#include <string.h>

#include "../src/gg.h"
#include "../src/vdp.h"
#include "../src/z80.h"

#define PAGES 4
#define RESULT 0xC100               /* the ROM stores what it saw from here */

#ifdef __TICE__
#define CODE_BUFFER ((uint8_t *)0xD40000)
#define REPORT ((char *)0xD52000)   /* "TEXT" then the report, for bench/cemu/run */
#define ROM ((uint8_t (*)[0x4000])0xD54000)
#else
static uint8_t rom_data[PAGES][0x4000];
#define CODE_BUFFER NULL
#define ROM rom_data
#endif

static int failures;
static char report[1024];
static size_t report_len;

static void put(uint8_t page, uint16_t offset, const uint8_t *code, size_t len)
{
    memcpy(&ROM[page][offset], code, len);
}

static uint8_t ram_at(uint16_t addr)
{
    return z80_rmap[addr >> 8].p[addr & 0xFF];
}

/*
 * Formatting by hand: the calculator's sprintf is in TI's boot code, which
 * bench/cemu/run replaces with RET instructions.
 */
static void add_text(const char *text)
{
    while (*text)
        report[report_len++] = *text++;
    report[report_len] = '\0';
}

static void add_hex(unsigned value)
{
    static const char digits[] = "0123456789ABCDEF";
    char text[3] = { digits[(value >> 4) & 15], digits[value & 15], '\0' };
    add_text(text);
}

static void expect(const char *what, unsigned got, unsigned want)
{
    add_text(got == want ? "ok   " : "FAIL ");
    add_text(what);
    add_text(" (got ");
    add_hex(got);
    add_text(", want ");
    add_hex(want);
    add_text(")\n");
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
        0x3A, 0x00, 0xC1,       /* LD A,(C100) */
        0x3C,                   /* INC A */
        0x32, 0x00, 0xC1,       /* LD (C100),A */
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
        0x32, 0x01, 0xC1,       /* LD (C101),A */
        0x3E, 0x5A,             /* LD A,5A */
        0x32, 0xF0, 0xE1,       /* LD (E1F0),A   write through the mirror */
        0x3A, 0xF0, 0xC1,       /* LD A,(C1F0) */
        0x32, 0x02, 0xC1,       /* LD (C102),A */
        0xDB, 0x00,             /* IN A,(00)     Start button */
        0x32, 0x03, 0xC1,       /* LD (C103),A */
        0xC3, 0xFC, 0x3F,       /* JP 3FFC       across slots 0 and 1 */
    };
    /* 3FFC: LD A,77 / LD (C104),A, whose address byte C1 is the first byte of slot 1. */
    static const uint8_t straddle0[] = { 0x3E, 0x77, 0x32, 0x04 };
    static const uint8_t straddle1[] = { 0xC1, 0xC3, 0x80, 0x01 };     /* ...; JP 0180 */
    /* 0180: copy NOPs to DFFC and a routine to C000, then run from DFFC into E000. */
    static const uint8_t ram_test[] = {
        0x21, 0x00, 0x03, 0x11, 0xFC, 0xDF, 0x01, 0x04, 0x00, 0xED, 0xB0,   /* LDIR 4 NOPs */
        0x21, 0x04, 0x03, 0x11, 0x00, 0xC0, 0x01, 0x08, 0x00, 0xED, 0xB0,   /* LDIR routine */
        0xC3, 0xFC, 0xDF,                                                   /* JP DFFC */
    };
    static const uint8_t ram_data[] = {
        0x00, 0x00, 0x00, 0x00,                                             /* DFFC-DFFF */
        0x3E, 0x99, 0x32, 0x05, 0xC1, 0xC3, 0x00, 0x02,                     /* LD A,99 / LD (C105),A / JP 0200 */
    };
    /* 0200: run code in slot 2 that switches slot 2's own bank. */
    static const uint8_t slot2_test[] = { 0xC3, 0x10, 0x80 };              /* JP 8010 */
    static const uint8_t page3_code[] = {
        0x3E, 0x55, 0x32, 0x06, 0xC1,                                       /* LD A,55 / LD (C106),A */
        0x3E, 0x02, 0x32, 0xFF, 0xFF,                                       /* LD A,2 / LD (FFFF),A */
    };
    static const uint8_t page2_code[] = {                                  /* continues at 801A */
        0x3E, 0x66, 0x32, 0x07, 0xC1,                                       /* LD A,66 / LD (C107),A */
        0xC3, 0x80, 0x02,                                                   /* JP 0280 */
    };
    static const uint8_t idle[] = {
        0xFB,                   /* EI */
        0x76,                   /* loop: HALT */
        0x18, 0xFD,             /* JR loop */
    };
    const uint8_t *pages[PAGES];

    memset(ROM, 0xFF, sizeof(uint8_t[PAGES][0x4000]));
    for (int p = 0; p < PAGES; p++)
    {
        ROM[p][0] = 0xA0 + p;
        pages[p] = ROM[p];
    }
    put(0, 0x0000, reset, sizeof reset);
    put(0, 0x0038, isr, sizeof isr);
    put(0, 0x0100, main_code, sizeof main_code);
    put(0, 0x3FFC, straddle0, sizeof straddle0);
    put(1, 0x0000, straddle1, sizeof straddle1);
    put(0, 0x0180, ram_test, sizeof ram_test);
    put(0, 0x0300, ram_data, sizeof ram_data);
    put(0, 0x0200, slot2_test, sizeof slot2_test);
    put(3, 0x0010, page3_code, sizeof page3_code);
    put(2, 0x001A, page2_code, sizeof page2_code);
    put(0, 0x0280, idle, sizeof idle);

    gg_init(pages, PAGES, CODE_BUFFER);
    gg.buttons = GG_START;
    for (int i = 0; i < 10; i++)
        gg_run_frame(NULL, 0, 0);

    expect("frame interrupts in 10 frames", ram_at(RESULT + 0), 10);
    expect("slot 2 read after bank switch", ram_at(RESULT + 1), 0xA3);
    expect("RAM mirror at E000", ram_at(RESULT + 2), 0x5A);
    expect("Start pressed reads bit 7 low", ram_at(RESULT + 3), 0x40);
    expect("instruction across slots 0 and 1", ram_at(RESULT + 4), 0x77);
    expect("code running from DFFF into E000", ram_at(RESULT + 5), 0x99);
    expect("code in slot 2 before its switch", ram_at(RESULT + 6), 0x55);
    expect("code in slot 2 after its switch", ram_at(RESULT + 7), 0x66);
    expect("mapper slot 2 register", gg.bank[2], 2);
    expect("VDP register 1", vdp.reg[1], 0x20);
    expect("palette color 0 low byte", vdp.cram[0], 0x34);
    expect("palette color 0 high byte", vdp.cram[1], 0x02);
    expect("CPU halted in main loop", z80.halted, 1);
    add_text(failures ? "FAILED\n" : "all passed\n");

#ifdef __TICE__
    memcpy(REPORT, "TEXT", 4);
    memcpy(REPORT + 4, report, report_len + 1);
#else
    fputs(report, stdout);
#endif
    return failures != 0;
}

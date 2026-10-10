/*
 * Measures how fast the Z80 core runs on a TI-84 Plus CE: it runs a fixed
 * number of cycles of a CP/M program (ZEXDOC) and times it with the
 * hardware timer, the same way GGCE measures Speed. Results go to
 * BENCH_RESULT so bench/cemu/run can read them back.
 */
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "../../src/z80.h"
#include "workload.h"

#define Z80_MEMORY ((uint8_t *)0xD40000)     /* 64 KB of LCD RAM */
#define BENCH_RESULT ((volatile uint32_t *)0xD60000)
#define BENCH_CYCLES 2000000UL               /* about 0.56 s of Game Gear time */
#define BDOS 0xFE00

void z80_mem_write(uint16_t addr, uint8_t value)
{
    Z80_MEMORY[addr] = value;
}

uint8_t z80_io_read(uint16_t port)
{
    (void)port;
    return 0xFF;
}

void z80_io_write(uint16_t port, uint8_t value)
{
    (void)port;
    (void)value;                    /* console output is ignored */
}

int main(void)
{
    uint8_t *mem = Z80_MEMORY;
    uint32_t total = 0;
    clock_t start, ticks;

    memset(mem, 0, 0x10000);
    memcpy(mem + 0x100, workload, sizeof workload);
    mem[0] = 0x76;                                      /* HALT if it exits */
    mem[5] = 0xC3; mem[6] = BDOS & 0xFF; mem[7] = BDOS >> 8;
    mem[BDOS] = 0xD3; mem[BDOS + 1] = 0x00; mem[BDOS + 2] = 0xC9;
    for (unsigned i = 0; i < 256; i++)
        z80_rmap[i].p = mem + i * 0x100;

    z80_reset();
    z80.pc.w = 0x100;
    z80.sp.w = BDOS;

    start = clock();
    while (total < BENCH_CYCLES)
        total += z80_run(228);
    ticks = clock() - start;

    BENCH_RESULT[0] = 0x4243484D;                       /* "MHCB": results valid */
    BENCH_RESULT[1] = total;
    BENCH_RESULT[2] = ticks;
    BENCH_RESULT[3] = z80.pc.w;
    return 0;
}

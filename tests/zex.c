/*
 * Runs a CP/M Z80 test program such as ZEXDOC or ZEXALL on the PC build of
 * src/z80.c. Only the two BDOS calls those programs use are provided.
 *
 *   ./zex zexdoc.com
 */
#include <stdio.h>
#include <stdlib.h>

#include "z80.h"

#define BDOS 0xFE00

static uint8_t mem[0x10000];
static int finished;

void z80_mem_write(uint16_t addr, uint8_t value)
{
    mem[addr] = value;
}

uint8_t z80_io_read(uint16_t port)
{
    (void)port;
    return 0xFF;
}

void z80_io_write(uint16_t port, uint8_t value)
{
    (void)value;
    if ((port & 0xFF) == 1)
    {
        finished = 1;
        return;
    }
    switch (z80.bc.b.l)
    {
    case 2:
        putchar(z80.de.b.l);
        break;
    case 9:
        for (uint16_t a = z80.de.w; mem[a] != '$'; a++)
            putchar(mem[a]);
        break;
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    FILE *f;
    long long total = 0;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s program.com\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f)
    {
        perror(argv[1]);
        return 2;
    }
    if (fread(mem + 0x100, 1, sizeof mem - 0x100, f) == 0)
    {
        fprintf(stderr, "%s is empty\n", argv[1]);
        return 2;
    }
    fclose(f);

    /* 0000: OUT (1),A; HALT ends the run. 0005: JP BDOS. BDOS: OUT (0),A; RET */
    mem[0] = 0xD3; mem[1] = 0x01; mem[2] = 0x76;
    mem[5] = 0xC3; mem[6] = BDOS & 0xFF; mem[7] = BDOS >> 8;
    mem[BDOS] = 0xD3; mem[BDOS + 1] = 0x00; mem[BDOS + 2] = 0xC9;

    for (int i = 0; i < 64; i++)
        z80_rmap[i] = mem + i * 0x400;

    z80_reset();
    z80.pc.w = 0x100;
    z80.sp.w = BDOS;
    while (!finished)
        total += z80_run(1000);

    printf("\n%lld cycles\n", total);
    return 0;
}

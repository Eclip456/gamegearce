#ifndef Z80_H
#define Z80_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Portable Z80 interpreter. It compiles both for the calculator and for a
 * PC, so tests/zex.c can check it against the ZEXDOC instruction test.
 * Register pairs assume a little-endian host (true for the eZ80 and x86).
 */

typedef union {
    uint16_t w;
    struct {
        uint8_t l, h;
    } b;
} z80_pair_t;

typedef struct {
    z80_pair_t af, bc, de, hl, ix, iy, sp, pc;
    z80_pair_t af2, bc2, de2, hl2;
    uint8_t i, r;
    uint8_t iff1, iff2, im;
    bool halted;
    bool ei_pending;                /* EI delays interrupts by one instruction */
    bool irq_line;                  /* level of the maskable interrupt line */
} z80_t;

extern z80_t z80;

/*
 * Reads go through 256 windows of 256 bytes, indexed by the address's high
 * byte, which the machine points at ROM pages or RAM. Writes, port reads and
 * port writes are machine functions.
 */
typedef struct {
    const uint8_t *p;
    uint8_t pad;                    /* 4-byte entries: the eZ80 indexes them with two adds */
} z80_window_t;

extern z80_window_t z80_rmap[256];
void z80_mem_write(uint16_t addr, uint8_t value);
uint8_t z80_io_read(uint16_t port);
void z80_io_write(uint16_t port, uint8_t value);

/*
 * Used only by the assembly core (src/z80core.s, built with Z80_ASM):
 *
 * z80_wmap: windows the core may write straight to (RAM). NULL windows go
 * through z80_mem_write.
 *
 * z80_cmap: code for each 8 KB region. Entry i is a base pointer such that
 * base + address is the code at that address; the region must be readable
 * in one piece to its end, followed by 0xED guard bytes if execution could
 * run past it into memory that isn't the next region. NULL entries are
 * filled in by calling z80_code_base(address), which returns the base.
 */
extern z80_window_t z80_wmap[256];
extern z80_window_t z80_cmap[8];
const uint8_t *z80_code_base(uint16_t addr);

void z80_reset(void);

/* Runs for at least `cycles` clock cycles; returns how many actually ran. */
int z80_run(int cycles);

#endif

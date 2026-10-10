/*
 * Differential CPU test: runs every Z80 instruction (all prefixes) from
 * several random states and checksums what it did. The PC build uses the
 * C core and writes the checksums to a file; the calculator build
 * (tests/calc) uses the assembly core and leaves them in memory for
 * bench/cemu/run to save. tests/cpucompare.py lists the differences.
 *
 * Each run starts with the instruction at 0x1000 followed by HALT, memory
 * full of HALT except a random data area, registers pointing into that
 * area, and branch targets inside the HALT area, so it stops right after.
 */
#include <stdint.h>
#include <string.h>

#include "../src/z80.h"

#define GROUPS 7                    /* none, CB, ED, DD, FD, DDCB, FDCB */
#define STATES 4                    /* random starting states per instruction */
#define CODE 0x1000
#define DATA 0x7F00                 /* random bytes: 0x7F00-0x9EFF */
#define FAST 0x80                   /* windows 0x80-0x9E: the assembly core writes them directly */
#define FAST_END 0x9F
#define STACK 0x9000
#define MAX_CYCLES 20000

#ifdef __TICE__
#define MEMORY ((uint8_t *)0xD40000)
#define RESULTS ((uint32_t *)0xD52000)
#define PROGRESS ((volatile uint8_t *)0xD51FF0)     /* group, op, state: for debugging hangs */
#else
static uint8_t memory[0x10000];
static uint32_t results[GROUPS * 256];
#define MEMORY memory
#define RESULTS results
#endif

#define POOL 0x2000
#define MAX_LOG 512

static uint32_t rng;
static uint32_t write_sum, io_sum;
static uint8_t pool[POOL];          /* random bytes copied into the data area */
static uint16_t write_log[MAX_LOG]; /* addresses to restore after each run */
static unsigned log_count;

static uint32_t next_random(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static uint32_t mix(uint32_t h, uint32_t v)
{
    h ^= v;
    h *= 0x01000193UL;
    return h;
}

static int fast_window(uint16_t addr)
{
    return (addr >> 8) >= FAST && (addr >> 8) < FAST_END;
}

/*
 * Writes inside the fast windows are checked through data_sum() instead,
 * since the assembly core makes them without calling this.
 */
void z80_mem_write(uint16_t addr, uint8_t value)
{
    MEMORY[addr] = value;
    if (fast_window(addr))
        return;
    if (log_count < MAX_LOG)
        write_log[log_count++] = addr;
    write_sum = mix(write_sum, (uint32_t)addr << 8 | value);
}

/* Fletcher-style: only 16-bit adds, which are cheap on the eZ80. */
static uint32_t data_sum(void)
{
    uint16_t s1 = 0, s2 = 0;
    const uint8_t *p = MEMORY + FAST * 0x100;

    for (unsigned i = 0; i < (FAST_END - FAST) * 0x100; i++)
    {
        s1 += p[i];
        s2 += s1;
    }
    return (uint32_t)s2 << 16 | s1;
}

uint8_t z80_io_read(uint16_t port)
{
    return (uint8_t)(port ^ (port >> 8) ^ 0x5A);
}

void z80_io_write(uint16_t port, uint8_t value)
{
    io_sum = mix(io_sum, (uint32_t)port << 8 | value);
}

const uint8_t *z80_code_base(uint16_t addr)
{
    (void)addr;
    return MEMORY;
}

static uint16_t data_pointer(void)
{
    return (uint16_t)(DATA + 0x80 + (next_random() & 0xFF));
}

static uint16_t halt_address(void)
{
    return (uint16_t)(0x2000 + (next_random() & 0x3FFF));
}

/* Bytes after the opcode: displacement, immediate, or address. */
static uint8_t operand_bytes(int group, uint8_t op)
{
    uint8_t x = op >> 6, z = op & 7;

    if (group == 1 || group == 5 || group == 6)
        return 0;
    if (group == 2)
        return (x == 1 && z == 3) ? 2 : 0;
    if (group == 3 || group == 4)
    {
        if (op == 0x36)
            return 2;
        if (op == 0x34 || op == 0x35 || (x == 1 && op != 0x76 && (z == 6 || ((op >> 3) & 7) == 6)) ||
            (x == 2 && z == 6))
            return 1;
        if (op == 0x21 || op == 0x22 || op == 0x2A)
            return 2;
    }
    if (op == 0xCB || op == 0xDD || op == 0xED || op == 0xFD)
        return 0;
    if (x == 0)
    {
        if (z == 6 || (z == 0 && op >= 0x10))
            return 1;
        if (z == 1 && !(op & 8))
            return 2;
        if (z == 2 && op >= 0x20)
            return 2;
        return 0;
    }
    if (x == 3)
    {
        if (z == 6 || op == 0xD3 || op == 0xDB)
            return 1;
        if (z == 2 || z == 4 || op == 0xC3 || op == 0xCD)
            return 2;
    }
    return 0;
}

static int is_branch_to_nn(int group, uint8_t op)
{
    uint8_t z = op & 7;
    /* DD/FD in front of a jump changes nothing, so steer those too. */
    return (group == 0 || group == 3 || group == 4) && op >= 0xC0 &&
           (z == 2 || z == 4 || op == 0xC3 || op == 0xCD);
}

static void set_state(int group, uint8_t op)
{
    static const uint8_t prefix[GROUPS][2] = {
        { 0, 0 }, { 0xCB, 0 }, { 0xED, 0 }, { 0xDD, 0 }, { 0xFD, 0 }, { 0xDD, 0xCB }, { 0xFD, 0xCB },
    };
    uint16_t pc = CODE;
    uint8_t n = operand_bytes(group, op);
    uint16_t jump = halt_address();
    int block = group == 2 && op >= 0xA0 && op <= 0xBB;

    /* Undo the last run's writes; the whole 64 KB is HALT-filled only once. */
    for (unsigned i = 0; i < log_count; i++)
        MEMORY[write_log[i]] = 0x76;
    if (log_count >= MAX_LOG)
        memset(MEMORY, 0x76, 0x10000);
    log_count = 0;
    {
        uint16_t offset = next_random() & (POOL - 1);
        memcpy(MEMORY + DATA, pool + offset, POOL - offset);
        memcpy(MEMORY + DATA + (POOL - offset), pool, offset);
    }
    for (uint16_t a = STACK - 64; a < STACK + 64; a += 2)
    {
        uint16_t target = halt_address();
        MEMORY[a] = (uint8_t)target;
        MEMORY[a + 1] = target >> 8;
    }

    for (int i = 0; i < 2 && prefix[group][i]; i++)
        MEMORY[pc++] = prefix[group][i];
    if (group >= 5)
        MEMORY[pc++] = (uint8_t)next_random();         /* displacement before the op */
    MEMORY[pc++] = op;
    for (uint8_t i = 0; i < n; i++)
        MEMORY[pc++] = (uint8_t)next_random();
    if (is_branch_to_nn(group, op))
    {
        MEMORY[pc - 2] = (uint8_t)jump;
        MEMORY[pc - 1] = jump >> 8;
    }
    MEMORY[pc] = 0x76;
    MEMORY[pc + 1] = 0x76;

    memset(&z80, 0, sizeof z80);
    z80.af.w = (uint16_t)next_random();
    z80.bc.w = (next_random() & 1) ? data_pointer() : (uint16_t)next_random();
    z80.de.w = (next_random() & 1) ? data_pointer() : (uint16_t)next_random();
    z80.hl.w = data_pointer();
    z80.ix.w = data_pointer();
    z80.iy.w = data_pointer();
    z80.sp.w = (uint16_t)(STACK + ((next_random() & 0x1F) * 2) - 32);
    z80.pc.w = CODE;
    z80.af2.w = (uint16_t)next_random();
    z80.bc2.w = (uint16_t)next_random();
    z80.de2.w = (uint16_t)next_random();
    z80.hl2.w = (uint16_t)next_random();
    z80.i = (uint8_t)next_random();
    z80.iff1 = z80.iff2 = next_random() & 1;
    z80.im = next_random() % 3;

    if (block)
        z80.bc.w = next_random() & 0x0F;               /* keep repeats short */
    if (group == 0 && op == 0xE9)
        z80.hl.w = halt_address();                    /* JP (HL) */
    if ((group == 3 || group == 4) && op == 0xE9)
    {
        z80.ix.w = halt_address();
        z80.iy.w = halt_address();
    }
}

static uint32_t state_sum(void)
{
    uint32_t h = 0x811C9DC5UL;

    h = mix(h, z80.af.w & 0xFFD7);                      /* flag bits 3 and 5 differ on the eZ80 */
    h = mix(h, z80.bc.w);
    h = mix(h, z80.de.w);
    h = mix(h, z80.hl.w);
    h = mix(h, z80.ix.w);
    h = mix(h, z80.iy.w);
    h = mix(h, z80.sp.w);
    h = mix(h, z80.pc.w);
    h = mix(h, z80.af2.w);
    h = mix(h, z80.bc2.w);
    h = mix(h, z80.de2.w);
    h = mix(h, z80.hl2.w);
    h = mix(h, (uint32_t)z80.i << 24 | (uint32_t)z80.iff1 << 16 | (uint32_t)z80.iff2 << 8 | z80.im);
    h = mix(h, z80.halted);
    h = mix(h, write_sum);
    h = mix(h, io_sum);
    h = mix(h, data_sum());
    return h;
}

static void run_all(void)
{
    for (unsigned i = 0; i < 256; i++)
    {
        z80_rmap[i].p = MEMORY + i * 0x100;
        z80_wmap[i].p = (i >= FAST && i < FAST_END) ? MEMORY + i * 0x100 : NULL;
    }
    for (unsigned i = 0; i < 8; i++)
        z80_cmap[i].p = MEMORY;
    memset(MEMORY, 0x76, 0x10000);
    rng = 12345;
    for (unsigned i = 0; i < POOL; i++)
        pool[i] = (uint8_t)next_random();

    for (int group = 0; group < GROUPS; group++)
    {
        for (unsigned op = 0; op < 256; op++)
        {
            uint32_t h = 0;

            for (int k = 0; k < STATES; k++)
            {
                int cycles = 0;

#ifdef PROGRESS
                PROGRESS[0] = (uint8_t)group;
                PROGRESS[1] = (uint8_t)op;
                PROGRESS[2] = (uint8_t)k;
#endif
                rng = 0x9E3779B9UL ^ ((uint32_t)group << 24 | op << 8 | (uint32_t)k);
                next_random();
                z80_reset();
                set_state(group, (uint8_t)op);
                write_sum = io_sum = 0;
                /* One cycle at a time adds up exact instruction timings in both cores. */
                while (!z80.halted && cycles < MAX_CYCLES)
                    cycles += z80_run(1);
                h = mix(h, state_sum());
                h = mix(h, (uint32_t)cycles);
            }
            RESULTS[group * 256 + op] = h;
        }
    }
}

#ifdef __TICE__
int main(void)
{
    run_all();
    return 0;
}
#else
#include <stdio.h>

int main(int argc, char **argv)
{
    FILE *f;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s results.bin\n", argv[0]);
        return 2;
    }
    run_all();
    f = fopen(argv[1], "wb");
    if (!f)
    {
        perror(argv[1]);
        return 2;
    }
    /* Little-endian, the same layout the calculator leaves in memory. */
    for (int i = 0; i < GROUPS * 256; i++)
        for (int b = 0; b < 4; b++)
            fputc((int)(results[i] >> (8 * b)) & 0xFF, f);
    fclose(f);
    return 0;
}
#endif

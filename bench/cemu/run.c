/*
 * Runs a CE program with no TI OS in CEmu's core: the flash is all RET
 * instructions, so the program's few OS calls return at once. Built for
 * bench/GGBENCH.8xp, which reports its result at BENCH_RESULT.
 *
 *   ./run ../bin/GGBENCH.8xp
 *
 * Build with CEMU_DIR pointing at a CEmu checkout (see Makefile).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asic.h"
#include "cpu.h"
#include "emu.h"
#include "mem.h"

#define USER_MEM 0xD1A881
#define STACK_TOP 0xD1A87E
#define SENTINEL 0xD3FF00           /* JR $ : where main returns to */
#define BENCH_RESULT 0xD60000
#define GG_CLOCK 3579545.0
#define TIMER_HZ 32768.0

static int verbose;

void gui_console_clear(void) {}

void gui_console_printf(const char *format, ...)
{
    va_list args;
    if (!verbose)
        return;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

void gui_console_err_printf(const char *format, ...)
{
    va_list args;
    if (!verbose)
        return;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

asic_rev_t gui_handle_reset(const boot_ver_t *boot_ver, asic_rev_t loaded_rev, asic_rev_t default_rev,
                            emu_device_t device, bool *python)
{
    (void)boot_ver; (void)loaded_rev; (void)device; (void)python;
    return default_rev;
}

void gui_debug_open(int reason, uint32_t data) { (void)reason; (void)data; }
void gui_debug_close(void) {}

static uint8_t *load_program(const char *path, size_t *len)
{
    static uint8_t file[0x20000];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f)
    {
        perror(path);
        exit(2);
    }
    n = fread(file, 1, sizeof file, f);
    fclose(f);
    /* 55-byte file header, 17-byte variable header, 2-byte size, then the program. */
    if (n < 76 || memcmp(file, "**TI83F*", 8) || file[59] != 0x06)
    {
        fprintf(stderr, "%s is not a protected program (.8xp)\n", path);
        exit(2);
    }
    *len = file[72] | file[73] << 8;
    return file + 74;
}

int main(int argc, char **argv)
{
    char rom_path[] = "/tmp/ggbench-rom-XXXXXX";
    uint8_t *program, *ram;
    size_t len;
    uint32_t *result;
    int fd, ms;

    if (argc > 2 && !strcmp(argv[1], "-v"))
    {
        verbose = 1;
        argv++;
        argc--;
    }
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s [-v] program.8xp\n", argv[0]);
        return 2;
    }
    program = load_program(argv[1], &len);

    /* A 4 MB flash image of RET instructions stands in for the OS. */
    fd = mkstemp(rom_path);
    {
        static uint8_t rom[0x400000];
        FILE *f = fdopen(fd, "wb");
        memset(rom, 0xC9, sizeof rom);
        fwrite(rom, 1, sizeof rom, f);
        fclose(f);
    }
    if (emu_load(EMU_DATA_ROM, rom_path) == EMU_STATE_INVALID)
    {
        fprintf(stderr, "CEmu could not load the stand-in ROM\n");
        return 1;
    }
    remove(rom_path);

    /* The EF 7B header sits just below userMem; code starts at userMem. */
    memcpy(phys_mem_ptr(USER_MEM - 2, len), program, len);
    ram = phys_mem_ptr(SENTINEL, 2);
    ram[0] = 0x18;
    ram[1] = 0xFE;
    ram = phys_mem_ptr(STACK_TOP - 3, 3);
    ram[0] = SENTINEL & 0xFF;
    ram[1] = (SENTINEL >> 8) & 0xFF;
    ram[2] = SENTINEL >> 16;
    memset(phys_mem_ptr(BENCH_RESULT, 16), 0, 16);

    cpu.IEF1 = cpu.IEF2 = 0;
    cpu.ADL = cpu.L = cpu.IL = 1;
    cpu.registers.SPL = STACK_TOP - 3;
    cpu.registers.IY = 0xD00080;
    cpu.registers.PC = USER_MEM;
    cpu_flush(cpu.registers.PC, 1);

    emu_set_run_rate(1000);
    for (ms = 0; ms < 600000 && cpu.registers.PC != SENTINEL; ms += 100)
        emu_run(100);

    result = phys_mem_ptr(BENCH_RESULT, 16);
    if (cpu.registers.PC != SENTINEL || result[0] != 0x4243484D)
    {
        fprintf(stderr, "program did not finish (PC %06X after %d ms)\n", cpu.registers.PC, ms);
        return 1;
    }
    printf("Z80 cycles %u, timer ticks %u, emulated PC %04X\n", result[1], result[2], result[3]);
    printf("Speed: %.1f%% of a real Game Gear (%.2f s of calculator time)\n",
           100.0 * (result[1] / GG_CLOCK) / (result[2] / TIMER_HZ), result[2] / TIMER_HZ);
    return 0;
}

#include "gg.h"

#include <string.h>

#include "render.h"
#include "vdp.h"
#include "z80.h"

gg_t gg;

static const uint8_t *const *rom_pages;
static uint16_t rom_page_count;
static uint16_t rom_page_mask;

static uint8_t ram[0x2000];
static uint8_t cart_ram[0x4000];    /* battery RAM some games map into slot 2 */
static int cycle_debt;              /* cycles the last line overran */

static uint8_t page_index(uint8_t value)
{
    uint16_t page = value & rom_page_mask;
    return (uint8_t)(page < rom_page_count ? page : page % rom_page_count);
}

/* Points the CPU's 256-byte read windows for one 16 KB slot at its bank. */
static void map_slot(uint8_t slot)
{
    const uint8_t *base;
    z80_window_t *window = &z80_rmap[slot * 0x40];
    uint8_t first = 0;

    if (slot == 2 && (gg.ram_control & 0x08))
        base = cart_ram;
    else
        base = rom_pages[page_index(gg.bank[slot])];

    /* The first 1 KB always shows page 0, so the reset and interrupt code stay put. */
    if (slot == 0)
        first = 4;
    for (uint8_t i = first; i < 0x40; i++)
        window[i].p = base + i * 0x100;
}

void z80_mem_write(uint16_t addr, uint8_t value)
{
    if (addr >= 0xC000)
    {
        ram[addr & 0x1FFF] = value;
        if (addr >= 0xFFFC)
        {
            if (addr == 0xFFFC)
            {
                gg.ram_control = value;
                map_slot(2);
            }
            else
            {
                gg.bank[addr - 0xFFFD] = value;
                map_slot(addr - 0xFFFD);
            }
        }
    }
    else if (addr >= 0x8000 && (gg.ram_control & 0x08))
        cart_ram[addr & 0x3FFF] = value;
}

static void update_irq(void)
{
    z80.irq_line = vdp_irq();
}

uint8_t z80_io_read(uint16_t port)
{
    uint8_t p = (uint8_t)port, value;

    if (p < 0x07)
    {
        switch (p)
        {
        case 0x00: /* Start button (active low), overseas NTSC console */
            return (gg.buttons & GG_START) ? 0x40 : 0xC0;
        case 0x01: return 0x7F;
        case 0x02: return 0xFF;
        case 0x05: return 0x00;
        default: return 0xFF;
        }
    }
    switch (p & 0xC1)
    {
    case 0x40: return vdp_vcounter();
    case 0x41: return 0;           /* H counter: only latched by light guns */
    case 0x80: return vdp_read_data();
    case 0x81:
        value = vdp_read_status();
        update_irq();
        return value;
    case 0xC0: /* controller: up down left right 1 2, active low */
        return (uint8_t)~(gg.buttons & 0x3F);
    default:
        return 0xFF;
    }
}

void z80_io_write(uint16_t port, uint8_t value)
{
    uint8_t p = (uint8_t)port;

    if (p < 0x07)
        return;                     /* link port and stereo control */
    switch (p & 0xC1)
    {
    case 0x80:
        vdp_write_data(value);
        break;
    case 0x81:
        vdp_write_control(value);
        update_irq();
        break;
    default:
        break;                      /* sound chip and memory control: ignored */
    }
}

void gg_init(const uint8_t *const *pages, uint16_t page_count)
{
    rom_pages = pages;
    rom_page_count = page_count;
    for (rom_page_mask = 1; rom_page_mask < page_count; rom_page_mask <<= 1)
        ;
    rom_page_mask--;

    memset(&gg, 0, sizeof gg);
    memset(ram, 0, sizeof ram);
    memset(cart_ram, 0, sizeof cart_ram);
    gg.bank[0] = 0;
    gg.bank[1] = 1;
    gg.bank[2] = 2;
    cycle_debt = 0;

    for (uint16_t i = 0; i < 4; i++)
        z80_rmap[i].p = rom_pages[0] + i * 0x100;
    for (uint16_t i = 0xC0; i < 0x100; i++)
        z80_rmap[i].p = ram + (i & 0x1F) * 0x100;
    for (uint8_t slot = 0; slot < 3; slot++)
        map_slot(slot);

    vdp_reset();
    z80_reset();
}

void gg_run_frame(uint8_t *screen, uint16_t pitch, uint8_t palette_base)
{
    for (uint16_t line = 0; line < VDP_LINES; line++)
    {
        int budget = GG_CYCLES_PER_LINE - cycle_debt;

        vdp_start_line(line);
        if (screen && line >= GG_FIRST_LINE && line < GG_FIRST_LINE + GG_HEIGHT)
        {
            render_line((uint8_t)(line - GG_FIRST_LINE), screen, palette_base);
            screen += pitch;
        }
        update_irq();
        cycle_debt = z80_run(budget) - budget;
    }
    gg.frames++;
}

#include "vdp.h"

#include <string.h>

vdp_t vdp;

void vdp_reset(void)
{
    memset(&vdp, 0, sizeof vdp);
    memset(vdp.tile_dirty, 1, sizeof vdp.tile_dirty);
    vdp.cram_dirty = true;
}

/* Called at the start of every scanline, before the CPU runs it. */
void vdp_start_line(uint16_t line)
{
    vdp.line = line;
    if (line == 0)
        vdp.vscroll = vdp.reg[9];

    /* The line counter counts down on active lines and one line after. */
    if (line <= 192)
    {
        if (vdp.line_counter-- == 0)
        {
            vdp.line_counter = vdp.reg[10];
            vdp.line_irq_pending = true;
        }
    }
    else
        vdp.line_counter = vdp.reg[10];

    if (line == 193)
        vdp.status |= 0x80;
}

bool vdp_irq(void)
{
    return ((vdp.status & 0x80) && (vdp.reg[1] & 0x20)) ||
           (vdp.line_irq_pending && (vdp.reg[0] & 0x10));
}

uint8_t vdp_vcounter(void)
{
    /* NTSC 192-line mode counts 00-DA, then jumps back to D5-FF. */
    return vdp.line <= 0xDA ? (uint8_t)vdp.line : (uint8_t)(vdp.line - 6);
}

uint8_t vdp_read_data(void)
{
    uint8_t value = vdp.read_buffer;

    vdp.second_byte = false;
    vdp.read_buffer = vdp.vram[vdp.addr];
    vdp.addr = (vdp.addr + 1) & 0x3FFF;
    return value;
}

uint8_t vdp_read_status(void)
{
    uint8_t value = vdp.status | 0x1F;

    vdp.status = 0;
    vdp.line_irq_pending = false;
    vdp.second_byte = false;
    return value;
}

void vdp_write_data(uint8_t value)
{
    vdp.second_byte = false;
    if (vdp.code == 3)
    {
        /* Palette writes take effect a whole 16-bit color at a time. */
        if (vdp.addr & 1)
        {
            vdp.cram[vdp.addr & 0x3E] = vdp.cram_latch;
            vdp.cram[vdp.addr & 0x3F] = value & 0x0F;
            vdp.cram_dirty = true;
        }
        else
            vdp.cram_latch = value;
    }
    else
    {
        vdp.vram[vdp.addr] = value;
        vdp.tile_dirty[vdp.addr >> 5] = 1;
    }
    vdp.read_buffer = value;
    vdp.addr = (vdp.addr + 1) & 0x3FFF;
}

void vdp_write_control(uint8_t value)
{
    if (!vdp.second_byte)
    {
        vdp.addr = (vdp.addr & 0x3F00) | value;
        vdp.second_byte = true;
        return;
    }

    vdp.second_byte = false;
    vdp.code = value >> 6;
    vdp.addr = (uint16_t)((value & 0x3F) << 8) | (vdp.addr & 0xFF);
    switch (vdp.code)
    {
    case 0:
        vdp.read_buffer = vdp.vram[vdp.addr];
        vdp.addr = (vdp.addr + 1) & 0x3FFF;
        break;
    case 2:
        if ((value & 0x0F) < 11)
            vdp.reg[value & 0x0F] = (uint8_t)vdp.addr;
        break;
    }
}

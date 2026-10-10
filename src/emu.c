#include "emu.h"

#include <graphx.h>
#include <keypadc.h>
#include <stdio.h>
#include <time.h>

#include "gg.h"
#include "vdp.h"
#include "z80.h"

#define COLOR_BG 0x00
#define COLOR_TEXT 0xFF
#define COLOR_DIM 0xB5
#define PALETTE_BASE 64             /* graphx palette entries used for the swatches */
#define REDRAW_FRAMES 15
#define GG_FPS_X100 5992            /* NTSC frame rate, 59.92 Hz */

static uint8_t read_buttons(void)
{
    uint8_t b = 0;

    kb_Scan();
    if (kb_IsDown(kb_KeyUp)) b |= GG_UP;
    if (kb_IsDown(kb_KeyDown)) b |= GG_DOWN;
    if (kb_IsDown(kb_KeyLeft)) b |= GG_LEFT;
    if (kb_IsDown(kb_KeyRight)) b |= GG_RIGHT;
    if (kb_IsDown(kb_Key2nd)) b |= GG_1;
    if (kb_IsDown(kb_KeyAlpha)) b |= GG_2;
    if (kb_IsDown(kb_KeyMode)) b |= GG_START;
    return b;
}

/* Game Gear color ----BBBBGGGGRRRR to the LCD's 1555 format. */
static uint16_t gg_color(const uint8_t *c)
{
    uint8_t r = c[0] & 0x0F, g = c[0] >> 4, b = c[1] & 0x0F;
    return (uint16_t)(((r << 1 | r >> 3) << 10) | ((g << 1 | g >> 3) << 5) | (b << 1 | b >> 3));
}

static void draw_debug(const rom_info_t *game, uint32_t frames, unsigned speed)
{
    char line[48];

    gfx_FillScreen(COLOR_BG);
    gfx_SetTextFGColor(COLOR_TEXT);
    gfx_PrintStringXY(game->title, 8, 8);
    gfx_SetTextFGColor(COLOR_DIM);
    gfx_PrintStringXY("Running - no picture yet (debug view)", 8, 20);
    gfx_SetTextFGColor(COLOR_TEXT);

    sprintf(line, "Frames: %lu", (unsigned long)frames);
    gfx_PrintStringXY(line, 8, 44);
    sprintf(line, "Speed:  %u%% of real Game Gear", speed);
    gfx_PrintStringXY(line, 8, 56);
    sprintf(line, "PC %04X  SP %04X  AF %04X%s", z80.pc.w, z80.sp.w, z80.af.w,
            z80.halted ? "  halt" : "");
    gfx_PrintStringXY(line, 8, 76);
    sprintf(line, "Banks %02X %02X %02X  RAM ctl %02X", gg.bank[0], gg.bank[1], gg.bank[2],
            gg.ram_control);
    gfx_PrintStringXY(line, 8, 88);
    sprintf(line, "VDP R0 %02X  R1 %02X  display %s", vdp.reg[0], vdp.reg[1],
            (vdp.reg[1] & 0x40) ? "on" : "off");
    gfx_PrintStringXY(line, 8, 100);

    gfx_PrintStringXY("Palette:", 8, 124);
    for (uint8_t i = 0; i < 32; i++)
    {
        gfx_palette[PALETTE_BASE + i] = gg_color(&vdp.cram[i * 2]);
        gfx_SetColor(PALETTE_BASE + i);
        gfx_FillRectangle(8 + (i & 15) * 19, 136 + (i >> 4) * 20, 17, 18);
    }

    gfx_SetTextFGColor(COLOR_DIM);
    gfx_PrintStringXY("arrows d-pad  2nd/alpha 1/2  mode start", 8, 214);
    gfx_PrintStringXY("[clear] quit", 8, 226);
    gfx_SwapDraw();
}

void emu_run(const rom_info_t *game, const uint8_t *const pages[])
{
    clock_t start;
    uint32_t frames_at_start = 0;
    unsigned speed = 0;

    gg_init(pages, game->page_count);
    start = clock();

    for (;;)
    {
        gg.buttons = read_buttons();
        if (kb_IsDown(kb_KeyClear))
            break;

        gg_run_frame();

        if (gg.frames % REDRAW_FRAMES == 0)
        {
            clock_t now = clock();
            unsigned long ticks = now - start;

            /* Emulated frames per second (x100), then as a percentage of 59.92. */
            if (ticks)
            {
                unsigned long fps_x100 = (gg.frames - frames_at_start) * CLOCKS_PER_SEC * 100UL / ticks;
                speed = (unsigned)(fps_x100 * 100UL / GG_FPS_X100);
            }
            draw_debug(game, gg.frames, speed);

            /* Restart the clock after drawing so the speed counts emulation only. */
            start = clock();
            frames_at_start = gg.frames;
        }
    }

    /* Wait for [clear] to be released so the game list doesn't see it. */
    do
        kb_Scan();
    while (kb_IsDown(kb_KeyClear));
}

#include "emu.h"

#include <fileioc.h>
#include <graphx.h>
#include <keypadc.h>
#include <stdio.h>
#include <sys/lcd.h>
#include <time.h>

#include "gg.h"
#include "render.h"
#include "vdp.h"
#include "z80.h"

#define BUFFER_BYTES (LCD_WIDTH * LCD_HEIGHT)   /* one 8-bit graphx buffer */
#define PALETTE_BASE 32             /* Game Gear colors use LCD palette entries 32-63 */
#define SCREEN_X 80                 /* where the 160x144 picture goes */
#define SCREEN_Y 40
#define STATUS_Y 200
#define STATUS_FRAMES 30
#define MAX_SKIP 8
#define GG_FPS_X100 5992            /* NTSC frame rate, 59.92 Hz */

#define COLOR_BLACK 0x00
#define COLOR_WHITE 0xFF
#define COLOR_UNUSED 0x01           /* never drawn, so text backgrounds show */
#define CODE_VAR "GGCECODE"         /* temporary RAM AppVar holding the code buffer */

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

static void update_palette(void)
{
    for (uint8_t i = 0; i < 32; i++)
        gfx_palette[PALETTE_BASE + i] = gg_color(&vdp.cram[i * 2]);
    vdp.cram_dirty = false;
}

static void draw_status(uint32_t frames, unsigned speed, uint8_t skip)
{
    char line[48];

    sprintf(line, "Frame %lu   Speed %u%%   Draw 1/%u   ", (unsigned long)frames, speed, skip);
    gfx_PrintStringXY(line, 8, STATUS_Y);
}

/*
 * The CPU core runs game code from a RAM copy of the ROM slots
 * (GG_CODE_BUFFER_SIZE bytes). It lives in a temporary AppVar so the OS
 * accounts for the RAM; NULL if there isn't enough free.
 */
static uint8_t *alloc_code_buffer(void)
{
    uint8_t *buffer = NULL;
    uint8_t handle;

    ti_Delete(CODE_VAR);            /* left over if the calculator reset mid-game */
    handle = ti_Open(CODE_VAR, "w");
    if (!handle)
        return NULL;
    if (ti_Resize(GG_CODE_BUFFER_SIZE, handle) > 0)
        buffer = ti_GetDataPtr(handle);
    ti_Close(handle);
    if (!buffer)
        ti_Delete(CODE_VAR);
    return buffer;
}

static void wait_for_key(void)
{
    do
        kb_Scan();
    while (kb_AnyKey());
    do
        kb_Scan();
    while (!kb_AnyKey());
    do
        kb_Scan();
    while (kb_AnyKey());
}

void emu_run(const rom_info_t *game, const uint8_t *const pages[])
{
    uint8_t *screen, *picture, *code_buffer;
    uint8_t old_fg, old_bg, old_transparent;
    uint8_t skip = 1, held = 0;
    uint32_t frames_at_start = 0;
    clock_t start;

    code_buffer = alloc_code_buffer();
    if (!code_buffer)
    {
        gfx_FillScreen(COLOR_BLACK);
        gfx_SetTextFGColor(COLOR_WHITE);
        gfx_PrintStringXY("Not enough free RAM to run the game.", 8, 56);
        gfx_PrintStringXY("It needs 48 KB: archive or delete", 8, 70);
        gfx_PrintStringXY("programs and variables in RAM.", 8, 84);
        gfx_PrintStringXY("Press any key", 8, 228);
        gfx_SwapDraw();
        wait_for_key();
        return;
    }

    /*
     * Draw straight to the visible screen. graphx's other buffer is free
     * while the game runs, so the decoded tile cache lives there.
     */
    gfx_SetDrawScreen();
    screen = (uint8_t *)lcd_UpBase;
    render_tile_cache = (screen == (uint8_t *)lcd_Ram) ? screen + BUFFER_BYTES : (uint8_t *)lcd_Ram;
    picture = screen + SCREEN_Y * LCD_WIDTH + SCREEN_X;

    gfx_FillScreen(COLOR_BLACK);
    old_fg = gfx_SetTextFGColor(COLOR_WHITE);
    old_bg = gfx_SetTextBGColor(COLOR_BLACK);
    old_transparent = gfx_SetTextTransparentColor(COLOR_UNUSED);
    gfx_PrintStringXY(game->title, 8, 8);
    gfx_PrintStringXY("2nd/alpha 1/2  mode start  +/- draw rate", 8, 216);
    gfx_PrintStringXY("[clear] quit", 8, 228);

    gg_init(pages, game->page_count, code_buffer);
    start = clock();

    for (;;)
    {
        uint8_t keys;

        gg.buttons = read_buttons();
        if (kb_IsDown(kb_KeyClear))
            break;

        /* +/- change how many frames run per frame drawn, once per press. */
        keys = (kb_IsDown(kb_KeyAdd) ? 1 : 0) | (kb_IsDown(kb_KeySub) ? 2 : 0);
        if ((keys & 1) && !(held & 1) && skip < MAX_SKIP)
            skip++;
        if ((keys & 2) && !(held & 2) && skip > 1)
            skip--;
        held = keys;

        if (vdp.cram_dirty)
            update_palette();
        gg_run_frame(gg.frames % skip == 0 ? picture : NULL, LCD_WIDTH, PALETTE_BASE);

        if (gg.frames % STATUS_FRAMES == 0)
        {
            unsigned long ticks = clock() - start;
            unsigned speed = 0;

            /* Emulated frames per second (x100), then as a percentage of 59.92. */
            if (ticks)
            {
                unsigned long fps_x100 = (gg.frames - frames_at_start) * CLOCKS_PER_SEC * 100UL / ticks;
                speed = (unsigned)(fps_x100 * 100UL / GG_FPS_X100);
            }
            draw_status(gg.frames, speed, skip);
            start = clock();
            frames_at_start = gg.frames;
        }
    }

    /* Wait for [clear] to be released so the game list doesn't see it. */
    do
        kb_Scan();
    while (kb_IsDown(kb_KeyClear));

    gfx_SetTextFGColor(old_fg);
    gfx_SetTextBGColor(old_bg);
    gfx_SetTextTransparentColor(old_transparent);
    gfx_SetDefaultPalette(gfx_8bpp);
    gfx_SetDrawBuffer();
    ti_Delete(CODE_VAR);
}

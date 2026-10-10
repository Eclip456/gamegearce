#include <fileioc.h>
#include <graphx.h>
#include <keypadc.h>
#include <stdio.h>
#include <string.h>
#include <sys/timers.h>

#include "rom.h"

#define COLOR_BG 0x00
#define COLOR_TEXT 0xFF
#define COLOR_HILITE 0x18
#define COLOR_DIM 0xB5

static rom_info_t games[ROM_MAX_GAMES];
static const uint8_t *rom_pages[ROM_MAX_PAGES];

/* Waits for every key to be released, then for a new key press. */
static void wait_key(void)
{
    do
        kb_Scan();
    while (kb_AnyKey());
    do
        kb_Scan();
    while (!kb_AnyKey());
}

static void draw_header(const char *subtitle)
{
    gfx_FillScreen(COLOR_BG);
    gfx_SetTextFGColor(COLOR_TEXT);
    gfx_SetTextScale(2, 2);
    gfx_PrintStringXY("GGCE", 8, 8);
    gfx_SetTextScale(1, 1);
    gfx_SetTextFGColor(COLOR_DIM);
    gfx_PrintStringXY(subtitle, 8, 28);
    gfx_SetTextFGColor(COLOR_TEXT);
}

static void draw_list(uint8_t count, uint8_t selected)
{
    char line[48];

    draw_header("Game Gear emulator - pick a game");
    for (uint8_t i = 0; i < count && i < 16; i++)
    {
        uint8_t y = 48 + i * 11;
        if (i == selected)
        {
            gfx_SetColor(COLOR_HILITE);
            gfx_FillRectangle(4, y - 2, 312, 11);
        }
        sprintf(line, "%-5s %s", games[i].prefix, games[i].title);
        gfx_PrintStringXY(line, 8, y);
    }
    gfx_SetTextFGColor(COLOR_DIM);
    gfx_PrintStringXY("[enter] load   [clear] quit", 8, 228);
    gfx_SetTextFGColor(COLOR_TEXT);
    gfx_SwapDraw();
}

static void show_message3(const char *title, const char *line1, const char *line2, const char *line3)
{
    draw_header(title);
    gfx_PrintStringXY(line1, 8, 56);
    if (line2)
        gfx_PrintStringXY(line2, 8, 70);
    if (line3)
        gfx_PrintStringXY(line3, 8, 84);
    gfx_SetTextFGColor(COLOR_DIM);
    gfx_PrintStringXY("Press any key", 8, 228);
    gfx_SwapDraw();
    wait_key();
}

static void show_message(const char *title, const char *line1, const char *line2)
{
    show_message3(title, line1, line2, NULL);
}

/* Archiving can show the OS "Garbage Collect?" prompt, which needs the OS screen mode. */
static void before_gc(void)
{
    gfx_End();
}

static void after_gc(void)
{
    gfx_Begin();
    gfx_SetDrawBuffer();
}

static void load_game(const rom_info_t *game)
{
    char line1[48], line2[48], line3[48];
    rom_error_t err;
    rom_status_t status;
    uint32_t crc;

    draw_header(game->title);
    gfx_PrintStringXY("Checking ROM pages...", 8, 56);
    gfx_SwapDraw();

    status = rom_load(game, rom_pages, &err);
    if (status != ROM_OK)
    {
        sprintf(line2, "Page AppVar: %sp%02X (%s)", game->prefix, err.page,
                err.archived ? "Archive" : "RAM");
        if (status == ROM_BAD_SIZE)
            sprintf(line3, "Size %u bytes, expected 16392", err.size);
        else if (status == ROM_BAD_HEADER)
            sprintf(line3, "Starts %02X %02X %02X %02X %02X %02X, size %u",
                    err.head[0], err.head[1], err.head[2], err.head[3],
                    err.head[4], err.head[5], err.size);
        else
            line3[0] = '\0';
        show_message3(game->title,
                      status == ROM_MISSING_PAGE ? "A ROM page is missing." :
                      status == ROM_NO_MEMORY ? "Not enough Archive space." :
                      "A ROM page is damaged.",
                      line2, line3[0] ? line3 : NULL);
        return;
    }

    crc = rom_crc32(rom_pages, game->page_count);
    sprintf(line1, "%u pages, %lu KB", game->page_count, (unsigned long)(game->rom_size / 1024));
    if (crc != game->crc32)
    {
        show_message(game->title, line1, "CRC mismatch - re-send the ROM files.");
        return;
    }

    /* The CPU and video emulation will start here. */
    show_message(game->title, line1, "ROM OK. Emulation core not written yet.");
}

int main(void)
{
    uint8_t count, selected = 0;

    gfx_Begin();
    gfx_SetDrawBuffer();
    gfx_SetTextTransparentColor(COLOR_HILITE);
    gfx_SetTextBGColor(COLOR_HILITE);
    ti_SetGCBehavior(before_gc, after_gc);

    count = rom_find_games(games, ROM_MAX_GAMES);
    if (count > 16)
        count = 16;

    if (!count)
    {
        show_message("No games found",
                     "Convert a .gg ROM with tools/gg2ce.py",
                     "and send all the .8xv files over.");
        gfx_End();
        return 0;
    }

    for (;;)
    {
        draw_list(count, selected);
        wait_key();
        if (kb_IsDown(kb_KeyClear))
            break;
        if (kb_IsDown(kb_KeyDown))
            selected = (selected + 1) % count;
        else if (kb_IsDown(kb_KeyUp))
            selected = (selected + count - 1) % count;
        else if (kb_IsDown(kb_KeyEnter) || kb_IsDown(kb_Key2nd))
            load_game(&games[selected]);
    }

    gfx_End();
    return 0;
}

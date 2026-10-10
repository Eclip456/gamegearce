/*
 * Checks the scanline renderer against hand-built VDP state: tiles, flips,
 * palettes, scrolling, sprites, and priority. Exits non-zero on failure.
 */
#include <stdio.h>
#include <string.h>

#include "render.h"
#include "vdp.h"

#define NAME_TABLE 0x3800
#define SAT 0x3F00

static uint8_t cache[RENDER_TILE_CACHE_SIZE];
static uint8_t out[GG_WIDTH];
static int failures;

/* Stores a tile whose pixel (x, y) has color color(x, y). */
static void make_tile(uint16_t tile, uint8_t (*color)(uint8_t x, uint8_t y))
{
    for (uint8_t y = 0; y < 8; y++)
        for (uint8_t plane = 0; plane < 4; plane++)
        {
            uint8_t bits = 0;
            for (uint8_t x = 0; x < 8; x++)
                if (color(x, y) & (1 << plane))
                    bits |= 0x80 >> x;
            vdp.vram[tile * 32 + y * 4 + plane] = bits;
        }
    vdp.tile_dirty[tile] = 1;
}

static uint8_t gradient(uint8_t x, uint8_t y) { return (x + y) & 15; }
static uint8_t solid5(uint8_t x, uint8_t y) { (void)x; (void)y; return 5; }

static void set_entry(uint8_t row, uint8_t col, uint16_t tile, uint8_t flags)
{
    uint16_t a = NAME_TABLE + (row * 32 + col) * 2;
    vdp.vram[a] = (uint8_t)tile;
    vdp.vram[a + 1] = (uint8_t)((tile >> 8) & 1) | flags;
}

static void check(const char *what, uint8_t line, const uint8_t *want, int n, int start)
{
    int ok = 1;

    render_line(line, out, 0);
    for (int i = 0; i < n; i++)
        if (out[start + i] != want[i])
            ok = 0;
    printf("%-40s %s", what, ok ? "ok" : "FAIL:");
    if (!ok)
        for (int i = 0; i < n; i++)
            printf(" %d", out[start + i]);
    printf("\n");
    failures += !ok;
}

int main(void)
{
    render_tile_cache = cache;
    vdp_reset();
    vdp.reg[1] = 0x40;              /* display on */
    vdp.reg[2] = 0xFF;              /* name table 3800 */
    vdp.reg[5] = 0xFF;              /* sprite table 3F00 */
    vdp.reg[6] = 0xFF;              /* sprite patterns from tile 256 */
    vdp.vram[SAT] = 0xD0;           /* no sprites yet */

    make_tile(1, gradient);
    make_tile(257, solid5);

    /* Visible line 0 is VDP line 24 = tile row 3; column 0 is tile column 6. */
    set_entry(3, 6, 1, 0);
    set_entry(3, 7, 1, 0x02 | 0x08);                    /* h-flip, sprite palette */
    set_entry(4, 6, 1, 0x04);                           /* v-flip */

    {
        const uint8_t want[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
        check("plain tile", 0, want, 8, 0);
    }
    {
        const uint8_t want[] = { 23, 22, 21, 20, 19, 18, 17, 16 };
        check("h-flip, second palette", 0, want, 8, 8);
    }
    {
        const uint8_t want[] = { 7, 8, 9, 10 };        /* row 7 of the tile: x + 7 */
        check("v-flip", 8, want, 4, 0);
    }

    vdp.reg[8] = 3;                                     /* scroll right by 3 */
    {
        const uint8_t want[] = { 0, 0, 0, 0, 1, 2 };
        check("horizontal scroll", 0, want, 6, 0);
    }
    vdp.reg[8] = 0;

    vdp.vscroll = 8;                                    /* line 0 now shows row 4 */
    {
        const uint8_t want[] = { 7, 8, 9, 10 };
        check("vertical scroll", 0, want, 4, 0);
    }
    vdp.vscroll = 0;

    /* Sprite 0 at the top-left of the visible area, using tile 257. */
    vdp.vram[SAT] = 23;
    vdp.vram[SAT + 1] = 0xD0;
    vdp.vram[SAT + 0x80] = 48;
    vdp.vram[SAT + 0x81] = 1;
    {
        const uint8_t want[] = { 21, 21, 21, 21, 21, 21, 21, 21, 23 };
        check("sprite over background", 0, want, 9, 0);
    }

    set_entry(3, 6, 1, 0x10);                           /* background priority */
    {
        const uint8_t want[] = { 21, 1, 2, 3 };         /* color 0 still shows the sprite */
        check("background priority", 0, want, 4, 0);
    }

    vdp.reg[1] = 0x00;
    vdp.reg[7] = 3;
    {
        const uint8_t want[] = { 19, 19, 19 };
        check("display off shows border color", 0, want, 3, 157);
    }

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures != 0;
}

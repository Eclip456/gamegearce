#include "render.h"

#include <string.h>

#include "vdp.h"

#define MAX_SPRITES_PER_LINE 8

uint8_t *render_tile_cache;

static const uint8_t pixel_mask[8] = { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 };

/* Background pixels that cover sprites: priority bit set and not color 0. */
static uint8_t bg_over_sprites[GG_WIDTH];
static uint8_t sprite_drawn[GG_WIDTH];

/* Converts a tile's four bitplanes to one byte per pixel. */
static const uint8_t *tile_pixels(uint16_t tile)
{
    uint8_t *dst = render_tile_cache + tile * 64;

    if (vdp.tile_dirty[tile])
    {
        const uint8_t *src = &vdp.vram[tile * 32];

        vdp.tile_dirty[tile] = 0;
        for (uint8_t row = 0; row < 8; row++, src += 4)
        {
            for (uint8_t x = 0; x < 8; x++)
            {
                uint8_t m = pixel_mask[x];
                *dst++ = ((src[0] & m) ? 1 : 0) | ((src[1] & m) ? 2 : 0) |
                         ((src[2] & m) ? 4 : 0) | ((src[3] & m) ? 8 : 0);
            }
        }
        dst -= 64;
    }
    return dst;
}

static void render_background(uint8_t vline, uint8_t *out, uint8_t palette_base)
{
    const uint8_t *table = &vdp.vram[(vdp.reg[2] & 0x0E) << 10];
    uint8_t hscroll = (vline < 16 && (vdp.reg[0] & 0x40)) ? 0 : vdp.reg[8];
    uint8_t bx = (uint8_t)(GG_FIRST_COLUMN - hscroll);
    uint8_t i = 0;

    while (i < GG_WIDTH)
    {
        uint8_t screen_column = (uint8_t)((GG_FIRST_COLUMN + i) / 8);
        uint16_t row = vline;
        const uint8_t *entry, *pixels;
        uint8_t hi, base, prio, fx, n;

        /* Register 0 bit 7 stops vertical scrolling in the rightmost 8 columns. */
        if (!(screen_column >= 24 && (vdp.reg[0] & 0x80)))
        {
            row += vdp.vscroll;
            if (row >= 224)
                row -= 224;
        }

        entry = &table[((row / 8) * 32 + bx / 8) * 2];
        hi = entry[1];
        pixels = tile_pixels((uint16_t)(entry[0] | (hi & 1) << 8)) +
                 ((hi & 0x04) ? 7 - (row & 7) : (row & 7)) * 8;
        base = palette_base + ((hi & 0x08) ? 16 : 0);
        prio = hi & 0x10;

        /* Draw from bx's position in this tile to the tile's end. */
        fx = bx & 7;
        n = 8 - fx;
        if (n > GG_WIDTH - i)
            n = GG_WIDTH - i;
        for (uint8_t k = 0; k < n; k++, fx++)
        {
            uint8_t c = (hi & 0x02) ? pixels[7 - fx] : pixels[fx];
            out[i + k] = base + c;
            bg_over_sprites[i + k] = prio && c;
        }
        i += n;
        bx += n;
    }
}

static void render_sprites(uint8_t vline, uint8_t *out, uint8_t palette_base)
{
    const uint8_t *sat = &vdp.vram[(vdp.reg[5] & 0x7E) << 7];
    uint8_t height = (vdp.reg[1] & 0x02) ? 16 : 8;
    uint16_t pattern_base = (vdp.reg[6] & 0x04) ? 256 : 0;
    int shift = (vdp.reg[0] & 0x08) ? 8 : 0;
    uint8_t count = 0;

    memset(sprite_drawn, 0, sizeof sprite_drawn);
    for (uint8_t s = 0; s < 64; s++)
    {
        int y = sat[s];
        int row;

        if (y == 0xD0)
            break;
        y += 1;
        if (y > 0xD0)
            y -= 256;
        row = vline - y;
        if (row < 0 || row >= height)
            continue;

        if (++count > MAX_SPRITES_PER_LINE)
        {
            vdp.status |= 0x40;
            break;
        }

        {
            int x = sat[0x80 + s * 2] - shift - GG_FIRST_COLUMN;
            uint16_t tile = pattern_base | sat[0x81 + s * 2];
            const uint8_t *pixels;

            if (height == 16)
            {
                tile &= ~1u;
                if (row >= 8)
                    tile++;
            }
            pixels = tile_pixels(tile) + (row & 7) * 8;

            for (uint8_t px = 0; px < 8; px++)
            {
                int sx = x + px;
                uint8_t c = pixels[px];

                if (sx < 0 || sx >= GG_WIDTH || !c)
                    continue;
                if (sprite_drawn[sx])
                {
                    vdp.status |= 0x20;   /* collision; the earlier sprite wins */
                    continue;
                }
                sprite_drawn[sx] = 1;
                if (!bg_over_sprites[sx])
                    out[sx] = palette_base + 16 + c;
            }
        }
    }
}

void render_line(uint8_t line, uint8_t *out, uint8_t palette_base)
{
    uint8_t vline = line + GG_FIRST_LINE;

    if (!(vdp.reg[1] & 0x40))
    {
        /* Display off: the whole line shows the border color. */
        memset(out, palette_base + 16 + (vdp.reg[7] & 0x0F), GG_WIDTH);
        return;
    }
    render_background(vline, out, palette_base);
    render_sprites(vline, out, palette_base);
}

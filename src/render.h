#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>

#define GG_WIDTH 160
#define GG_HEIGHT 144
#define GG_FIRST_LINE 24            /* the LCD shows lines 24-167 of the VDP's 192 */
#define GG_FIRST_COLUMN 48          /* and columns 48-207 of its 256 */

#define RENDER_TILE_CACHE_SIZE (512 * 64)

/*
 * Decoded tiles: one byte (color 0-15) per pixel, 64 bytes per tile. The
 * front end points this at RENDER_TILE_CACHE_SIZE bytes before rendering.
 */
extern uint8_t *render_tile_cache;

/*
 * Draws one visible line (0-143) into out[0..159] as palette indices:
 * palette_base + 0-31, where 0-15 are background colors and 16-31 sprite
 * colors (the order of the Game Gear's palette RAM).
 */
void render_line(uint8_t line, uint8_t *out, uint8_t palette_base);

#endif

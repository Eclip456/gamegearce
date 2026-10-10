#ifndef GG_H
#define GG_H

#include <stdint.h>

#define GG_CYCLES_PER_LINE 228      /* 3.58 MHz / 262 lines / 60 Hz */

/* Buttons, set by the front end before each frame. Bit set = held. */
#define GG_UP    0x01
#define GG_DOWN  0x02
#define GG_LEFT  0x04
#define GG_RIGHT 0x08
#define GG_1     0x10
#define GG_2     0x20
#define GG_START 0x80

typedef struct {
    uint8_t buttons;
    uint8_t bank[3];                /* ROM page in each 16 KB slot */
    uint8_t ram_control;            /* mapper register FFFC */
    uint32_t frames;
} gg_t;

extern gg_t gg;

/* pages[i] points at ROM page i (16 KB each). */
void gg_init(const uint8_t *const *pages, uint16_t page_count);
void gg_run_frame(void);

#endif

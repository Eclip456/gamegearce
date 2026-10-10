#ifndef VDP_H
#define VDP_H

#include <stdbool.h>
#include <stdint.h>

#define VDP_LINES 262               /* NTSC */

/* The Game Gear's video chip: a Sega Master System VDP with a 12-bit palette. */
typedef struct {
    uint8_t vram[0x4000];
    uint8_t cram[64];               /* 32 colors, little endian ----BBBBGGGGRRRR */
    uint8_t reg[16];
    uint8_t status;                 /* bit 7: frame interrupt pending */
    bool line_irq_pending;
    bool second_byte;               /* control port is waiting for byte 2 */
    uint8_t code;                   /* 0 VRAM read, 1 VRAM write, 2 register, 3 CRAM */
    uint16_t addr;
    uint8_t read_buffer;
    uint8_t cram_latch;
    uint8_t line_counter;
    uint8_t vscroll;                /* register 9, latched at the start of each frame */
    uint16_t line;
    bool cram_dirty;                /* palette changed since the front end last looked */
    uint8_t tile_dirty[512];        /* tile pattern changed since it was last decoded */
} vdp_t;

extern vdp_t vdp;

void vdp_reset(void);
void vdp_start_line(uint16_t line);
bool vdp_irq(void);

uint8_t vdp_read_data(void);
uint8_t vdp_read_status(void);
uint8_t vdp_vcounter(void);
void vdp_write_data(uint8_t value);
void vdp_write_control(uint8_t value);

#endif

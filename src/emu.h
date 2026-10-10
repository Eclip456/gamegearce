#ifndef EMU_H
#define EMU_H

#include <stdint.h>

#include "rom.h"

/*
 * Runs a game until [clear] is pressed, drawing its 160x144 picture in the
 * middle of the screen with a status line (frame count, speed) below.
 */
void emu_run(const rom_info_t *game, const uint8_t *const pages[]);

#endif

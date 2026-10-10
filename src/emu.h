#ifndef EMU_H
#define EMU_H

#include <stdint.h>

#include "rom.h"

/*
 * Runs a game until [clear] is pressed. There is no picture yet: the screen
 * shows a debug view (frame count, speed, CPU state, palette) that proves
 * the game's code is running.
 */
void emu_run(const rom_info_t *game, const uint8_t *const pages[]);

#endif

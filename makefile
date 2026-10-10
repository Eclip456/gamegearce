# ----------------------------
# Makefile Options
# ----------------------------

NAME = GGCE
DESCRIPTION = "Game Gear emulator"
COMPRESSED = NO

CFLAGS = -Wall -Wextra -Oz -DZ80_ASM
CXXFLAGS = -Wall -Wextra -Oz -DZ80_ASM

# ----------------------------

include $(shell cedev-config --makefile)

# ----------------------------
# Makefile Options
# ----------------------------

NAME = GGCE
DESCRIPTION = "Game Gear emulator"
COMPRESSED = NO

CFLAGS = -Wall -Wextra -O3
CXXFLAGS = -Wall -Wextra -O3

# ----------------------------

include $(shell cedev-config --makefile)

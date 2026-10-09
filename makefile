# ----------------------------
# Makefile Options
# ----------------------------

NAME = GGCE
DESCRIPTION = "Game Gear emulator"
COMPRESSED = NO

CFLAGS = -Wall -Wextra -Oz
CXXFLAGS = -Wall -Wextra -Oz

# ----------------------------

include $(shell cedev-config --makefile)

# ----------------------------
# Makefile Options
# ----------------------------

NAME = GGBENCH
DESCRIPTION = "GGCE CPU benchmark"
COMPRESSED = NO
ARCHIVED = NO

CFLAGS = -Wall -Wextra -Oz
CXXFLAGS = -Wall -Wextra -Oz

# ----------------------------

include $(shell cedev-config --makefile)

# The benchmark runs ZEXDOC, which isn't stored in this repository.
# Generate src/workload.h first:  python3 cemu/embed.py path/to/zexdoc.com

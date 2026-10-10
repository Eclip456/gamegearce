# Builds `run`, which runs GGBENCH.8xp in CEmu's core without a TI OS.
#   git clone https://github.com/CE-Programming/CEmu
#   make -C CEmu/core lib
#   make CEMU_DIR=path/to/CEmu

CEMU_DIR ?= ../../../CEmu
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

run: run.c $(CEMU_DIR)/core/libcemucore.a
	$(CC) $(CFLAGS) -I$(CEMU_DIR)/core -o $@ run.c $(CEMU_DIR)/core/libcemucore.a -lm -lpthread

clean:
	rm -f run

.PHONY: clean

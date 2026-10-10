# PC builds of the emulator core, for testing without a calculator.
#   make -C tests            build
#   make -C tests check      run the Game Gear and renderer tests
#   ./tests/zex zexdoc.com   run a Z80 instruction exerciser

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS += -I../src

all: zex ggtest rendertest

zex: zex.c ../src/z80.c ../src/z80.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ zex.c ../src/z80.c

ggtest: ggtest.c ../src/gg.c ../src/vdp.c ../src/z80.c ../src/render.c ../src/*.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ ggtest.c ../src/gg.c ../src/vdp.c ../src/z80.c ../src/render.c

rendertest: rendertest.c ../src/render.c ../src/vdp.c ../src/*.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ rendertest.c ../src/render.c ../src/vdp.c

check: ggtest rendertest
	./ggtest
	./rendertest

clean:
	rm -f zex ggtest rendertest

.PHONY: all check clean

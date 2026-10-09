# gamegearce

My attempt at making a Sega Game Gear emulator for the TI-84 Plus CE.

**Status:** early. The calculator program (`GGCE`) can find converted games,
check that every ROM page arrived intact, and verify the ROM's CRC-32. The
emulation core (CPU, video, input) isn't written yet. There will be no sound:
the calculator has no speaker.

## Converting a game

ROMs can't be sent to the calculator as-is. Convert a `.gg` file with Python 3:

```sh
python3 tools/gg2ce.py "Sonic the Hedgehog.gg" --name SONIC --title "Sonic" -o out
```

This writes `SONIC.8xv` (the header) plus one `SONICpXX.8xv` per 16 KB ROM
page. Send **all** of them to the calculator with TI Connect CE, into Archive.

- `--name` is the on-calc prefix: 1-5 characters, uppercase letters and
  digits, starting with a letter. Each game needs a different one.
- `--title` is what shows up in the game list.

Only use ROMs of games you own, or homebrew.

## Building

Install the [CE C toolchain](https://github.com/CE-Programming/toolchain)
(CEdev), put its `bin` folder on your `PATH`, then:

```sh
make
```

The program ends up in `bin/GGCE.8xp`. Calculators on OS 5.5 or newer need a
jailbreak such as arTIfiCE to run assembly/C programs.

Converter tests:

```sh
cd tools && python3 -m unittest -v
```

## How the emulator will work

The Game Gear's CPU is a Z80, and the calculator's eZ80 can run Z80 code
natively in its Z80 mode. The plan is to run game code directly instead of
interpreting it, and to emulate only the parts that differ:

1. **Memory map:** the Sega mapper's three 16 KB ROM slots plus 8 KB of RAM,
   inside a 64 KB block the eZ80 runs in Z80 mode.
2. **I/O ports:** catch the game's `IN`/`OUT` instructions for the video chip
   (VDP), controller, and mapper.
3. **Video:** tiles, sprites, and the 4096-color palette, drawn at 160x144 in
   the middle of the 320x240 screen.
4. **Input:** D-pad on the arrow keys, buttons 1/2 on `2nd`/`alpha`, Start on
   `mode`.
5. **Speed:** frame skipping and tuning on real hardware.

## ROM file format

Header AppVar (`PREFIX`):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 7 | `GGCEHDR` |
| 7 | 1 | format version (1) |
| 8 | 1 | system (0 = Game Gear) |
| 9 | 1 | page count (0 means 256) |
| 10 | 4 | ROM size in bytes, little endian |
| 14 | 4 | CRC-32 of the padded ROM |
| 18 | 32 | title, NUL-terminated |

Page AppVar (`PREFIXpXX`, XX = page number in hex): `GGPG`, page number,
format version, 2 reserved bytes, then 16384 bytes of ROM.

#!/usr/bin/env python3
"""Convert a Sega Game Gear ROM (.gg) into TI-84 Plus CE AppVars (.8xv).

The ROM is split into 16 KB pages, matching the Game Gear's bank size, so the
emulator can find any bank by name without searching. For a game prefix of
"SONIC" this writes:

    SONIC.8xv      header: magic, title, page count, ROM size, CRC-32
    SONICp00.8xv   ROM page 0
    SONICp01.8xv   ROM page 1
    ...

The prefix is limited to 5 uppercase letters/digits so the page names
(prefix + "p" + two hex digits) fit the calculator's 8-character limit.
The lowercase "p" can never appear in a prefix, so one game's pages can't
collide with another game's header.

Send every generated file to the calculator (Archive is recommended).
"""

import argparse
import os
import re
import struct
import sys
import zlib

PAGE_SIZE = 0x4000
MAX_PAGES = 256  # page numbers are one byte; real Game Gear ROMs top out at 64
FORMAT_VERSION = 1

HEADER_MAGIC = b"GGCEHDR"
PAGE_MAGIC = b"GGPG"
TITLE_LEN = 32

SYSTEM_GAME_GEAR = 0

APPVAR_TYPE = 0x15
FLAG_ARCHIVED = 0x80
MAX_APPVAR_DATA = 0xFFEB  # largest data section the OS accepts for one variable

PREFIX_RE = re.compile(r"^[A-Z][A-Z0-9]{0,4}$")


class ConvertError(Exception):
    pass


def make_8xv(name, payload, archived=True, comment="Game Gear ROM for GGCE"):
    """Build the bytes of a TI-83 Premium CE/TI-84 Plus CE AppVar file."""
    if len(payload) > MAX_APPVAR_DATA:
        raise ConvertError(f"AppVar {name} is too large ({len(payload)} bytes)")
    raw_name = name.encode("ascii")
    if not 1 <= len(raw_name) <= 8:
        raise ConvertError(f"invalid AppVar name {name!r}")

    var_data = struct.pack("<H", len(payload)) + payload
    entry = (
        struct.pack("<HH", 0x0D, len(var_data))
        + bytes([APPVAR_TYPE])
        + raw_name.ljust(8, b"\0")
        + bytes([0, FLAG_ARCHIVED if archived else 0])
        + struct.pack("<H", len(var_data))
        + var_data
    )
    header = b"**TI83F*\x1a\x0a\x00" + comment.encode("ascii")[:42].ljust(42, b"\0")
    checksum = sum(entry) & 0xFFFF
    return header + struct.pack("<H", len(entry)) + entry + struct.pack("<H", checksum)


def default_prefix(rom_path):
    base = os.path.splitext(os.path.basename(rom_path))[0].upper()
    letters = re.sub(r"[^A-Z0-9]", "", base)
    letters = letters.lstrip("0123456789")
    return (letters or "GAME")[:5]


def page_name(prefix, index):
    return f"{prefix}p{index:02X}"


def split_rom(rom):
    """Strip a 512-byte copier header if present and pad to whole pages."""
    if len(rom) % PAGE_SIZE == 512:
        rom = rom[512:]
    if not rom:
        raise ConvertError("ROM file is empty")
    if len(rom) % PAGE_SIZE:
        rom = rom + b"\xff" * (PAGE_SIZE - len(rom) % PAGE_SIZE)
    pages = [rom[i:i + PAGE_SIZE] for i in range(0, len(rom), PAGE_SIZE)]
    if len(pages) > MAX_PAGES:
        raise ConvertError(f"ROM is too large ({len(rom)} bytes, max {MAX_PAGES * PAGE_SIZE})")
    return rom, pages


def build_header(title, rom, page_count):
    title_bytes = title.encode("ascii", "replace")[:TITLE_LEN - 1].ljust(TITLE_LEN, b"\0")
    return (
        HEADER_MAGIC
        + bytes([FORMAT_VERSION, SYSTEM_GAME_GEAR, page_count & 0xFF])
        + struct.pack("<I", len(rom))
        + struct.pack("<I", zlib.crc32(rom) & 0xFFFFFFFF)
        + title_bytes
    )


def build_page(index, data):
    return PAGE_MAGIC + bytes([index, FORMAT_VERSION, 0, 0]) + data


def convert(rom_bytes, prefix, title, archived=True):
    """Return a list of (filename, file_bytes) for a ROM."""
    if not PREFIX_RE.match(prefix):
        raise ConvertError(
            f"name {prefix!r} must be 1-5 characters: an uppercase letter, then letters or digits"
        )
    rom, pages = split_rom(rom_bytes)
    files = [(f"{prefix}.8xv", make_8xv(prefix, build_header(title, rom, len(pages)), archived))]
    for i, data in enumerate(pages):
        name = page_name(prefix, i)
        files.append((f"{name}.8xv", make_8xv(name, build_page(i, data), archived)))
    return files


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("rom", help="Game Gear ROM file (.gg)")
    parser.add_argument("-n", "--name", help="on-calc name prefix, 1-5 chars (default: from file name)")
    parser.add_argument("-t", "--title", help="title shown in the game list (default: file name)")
    parser.add_argument("-o", "--outdir", default=".", help="output directory (default: current)")
    parser.add_argument("--ram", action="store_true", help="mark AppVars for RAM instead of Archive")
    args = parser.parse_args(argv)

    if not args.rom.lower().endswith(".gg"):
        print(f"warning: {args.rom} doesn't end in .gg; converting anyway", file=sys.stderr)

    prefix = (args.name or default_prefix(args.rom)).upper()
    title = args.title or os.path.splitext(os.path.basename(args.rom))[0]
    with open(args.rom, "rb") as f:
        rom = f.read()

    try:
        files = convert(rom, prefix, title, archived=not args.ram)
    except ConvertError as e:
        parser.error(str(e))

    os.makedirs(args.outdir, exist_ok=True)
    for filename, data in files:
        with open(os.path.join(args.outdir, filename), "wb") as f:
            f.write(data)
    print(f"{title}: {len(files) - 1} pages -> {len(files)} files named {prefix}*.8xv in {args.outdir}")
    print("Send all of them to the calculator, then run GGCE.")


if __name__ == "__main__":
    main()

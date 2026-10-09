import os
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

import gg2ce


def parse_8xv(blob):
    """Return (name, archived, payload) after checking the file's structure."""
    assert blob[:11] == b"**TI83F*\x1a\x0a\x00"
    (data_len,) = struct.unpack_from("<H", blob, 53)
    entry = blob[55:55 + data_len]
    (checksum,) = struct.unpack_from("<H", blob, 55 + data_len)
    assert len(blob) == 57 + data_len
    assert checksum == sum(entry) & 0xFFFF
    hdr_len, var_len = struct.unpack_from("<HH", entry, 0)
    assert hdr_len == 0x0D and entry[4] == gg2ce.APPVAR_TYPE
    name = entry[5:13].rstrip(b"\0").decode()
    archived = entry[14] == gg2ce.FLAG_ARCHIVED
    assert struct.unpack_from("<H", entry, 15)[0] == var_len
    (size,) = struct.unpack_from("<H", entry, 17)
    payload = entry[19:]
    assert size == len(payload) == var_len - 2
    return name, archived, payload


class ConvertTests(unittest.TestCase):
    def rom(self, pages):
        return bytes((i * 7 + i // 251) & 0xFF for i in range(pages * gg2ce.PAGE_SIZE))

    def test_header_and_pages(self):
        rom = self.rom(4)
        files = gg2ce.convert(rom, "SONIC", "Sonic the Hedgehog")
        self.assertEqual([f for f, _ in files],
                         ["SONIC.8xv", "SONICp00.8xv", "SONICp01.8xv", "SONICp02.8xv", "SONICp03.8xv"])

        name, archived, hdr = parse_8xv(files[0][1])
        self.assertEqual((name, archived), ("SONIC", True))
        self.assertEqual(hdr[:7], b"GGCEHDR")
        self.assertEqual(hdr[7:10], bytes([1, 0, 4]))
        self.assertEqual(struct.unpack_from("<II", hdr, 10), (len(rom), zlib.crc32(rom)))
        self.assertEqual(hdr[18:].rstrip(b"\0"), b"Sonic the Hedgehog")

        joined = b""
        for i, (_, blob) in enumerate(files[1:]):
            name, _, page = parse_8xv(blob)
            self.assertEqual(name, f"SONICp{i:02X}")
            self.assertEqual(page[:6], b"GGPG" + bytes([i, 1]))
            joined += page[8:]
        self.assertEqual(joined, rom)

    def test_pads_partial_page_and_strips_copier_header(self):
        files = gg2ce.convert(b"\x00" * 512 + b"\xaa" * 100, "A", "t")
        self.assertEqual(len(files), 2)
        page = parse_8xv(files[1][1])[2][8:]
        self.assertEqual(len(page), gg2ce.PAGE_SIZE)
        self.assertEqual(page[:612], b"\x00" * 512 + b"\xaa" * 100)

        files = gg2ce.convert(b"\x11" * 512 + b"\x22" * gg2ce.PAGE_SIZE, "A", "t")
        self.assertEqual(parse_8xv(files[1][1])[2][8:], b"\x22" * gg2ce.PAGE_SIZE)

    def test_ram_flag(self):
        files = gg2ce.convert(self.rom(1), "AB", "t", archived=False)
        self.assertFalse(parse_8xv(files[0][1])[1])

    def test_bad_prefix(self):
        for bad in ["", "1ABC", "TOOLONG", "AB-C", "abc"]:
            with self.assertRaises(gg2ce.ConvertError):
                gg2ce.convert(self.rom(1), bad, "t")

    def test_default_prefix(self):
        self.assertEqual(gg2ce.default_prefix("roms/Sonic The Hedgehog (W).gg"), "SONIC")
        self.assertEqual(gg2ce.default_prefix("2048.gg"), "GAME")

    @unittest.skipUnless(shutil.which("convbin"), "convbin from the CE toolchain not on PATH")
    def test_matches_convbin(self):
        payload = gg2ce.build_page(3, self.rom(1))
        ours = gg2ce.make_8xv("TESTp03", payload, archived=True, comment="")
        with tempfile.TemporaryDirectory() as d:
            src, out = os.path.join(d, "in.bin"), os.path.join(d, "out.8xv")
            with open(src, "wb") as f:
                f.write(payload)
            subprocess.run(["convbin", "-r", "-k", "8xv", "-n", "TESTp03", "-b", "",
                            "-i", src, "-o", out, "-l", "0"], check=True)
            with open(out, "rb") as f:
                theirs = f.read()
        # Ignore the comment field; compare everything else byte for byte.
        self.assertEqual(ours[:11], theirs[:11])
        self.assertEqual(ours[53:], theirs[53:])


if __name__ == "__main__":
    unittest.main()

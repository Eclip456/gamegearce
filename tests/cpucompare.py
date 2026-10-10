#!/usr/bin/env python3
"""Compares tests/cputest results from the C core and the assembly core.

    ./cpucompare.py c.bin asm.bin
"""
import struct
import sys

GROUPS = ["", "CB ", "ED ", "DD ", "FD ", "DD CB d ", "FD CB d "]

a, b = (open(p, "rb").read() for p in sys.argv[1:3])
n = len(GROUPS) * 256
ca, cb = struct.unpack("<%dI" % n, a[:n * 4]), struct.unpack("<%dI" % n, b[:n * 4])
# Differences on purpose: LD A,R reads the eZ80's own refresh counter.
EXPECTED = {2 * 256 + 0x5F}

bad = [i for i in range(n) if ca[i] != cb[i] and i not in EXPECTED]
for i in bad:
    print("differs: %s%02X" % (GROUPS[i // 256], i % 256))
print("%d of %d instructions differ" % (len(bad), n))
sys.exit(1 if bad else 0)

#!/usr/bin/env python3
"""Extract the raw ADPCM blob out of a generated _adpcm.c file.

The C array emitted by mp3_to_dsm.py --format adpcm IS the loadable blob
(header + blocks), byte for byte - this just unwraps it so it can be
used with 'tqv.py load' / 'tqv.py send'.  No dependencies.

Usage: adpcm_c2bin.py <song_adpcm.c> <out.bin>
"""
import re
import struct
import sys

if len(sys.argv) != 3:
    sys.exit(__doc__)

data = bytearray()
in_arr = False
for line in open(sys.argv[1]):
    if "_adpcm[]" in line and "{" in line:
        in_arr = True
        continue
    if in_arr:
        if "};" in line:
            break
        data += bytes(int(t, 16) for t in re.findall(r"0x([0-9a-fA-F]{2})", line))
if not in_arr:
    sys.exit("error: no _adpcm[] array found - is this an ADPCM .c file?")

total, rate, blk = struct.unpack_from("<III", data, 0)
if blk != 1024:
    sys.exit(f"error: block size {blk} != 1024 - not an ADPCM blob")
open(sys.argv[2], "wb").write(data)
print(f"wrote {sys.argv[2]}: {len(data)} bytes, {total} samples at "
      f"{rate} Hz ({total / rate:.1f} s)")

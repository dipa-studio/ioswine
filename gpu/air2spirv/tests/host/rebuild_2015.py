#!/usr/bin/env python3
"""rebuild_2015.py - rebuild the 2015 HelloTriangle .metallib from the
hex dump printed in zhuowei's writeup, and write it to the given path.

Source: https://worthdoingbadly.com/metalbitcode/ (2018-08-25).
The article prints the first 0x1a0 bytes of the file (header, function
list, metadata sections, start of the first bitcode module) and states
the rest: file size 0x1532, first bitcode module at 0x182 size 0xaf0,
second module at 0xc72 size 0x8c0. The bitcode bodies past 0x19f are
not printed in the article, so this script zero-fills them.

Consequences, all expected and all checked by tests/host/ml_2015_check.c
and tools/metallib_scan.py:
  - the container parses exactly: 2 functions, vertexShader and
    fragmentShader, with the OFFT / MDSZ / VERS / HASH values from the
    printed rows;
  - the first module's wrapper magic sits exactly at the header's
    bitcode offset 0x182 (there is no 8-byte anomaly; an earlier draft
    of the research doc misread the dump and claimed 0x17a, which holds
    04 00 00 00, the size field of the second private-metadata record);
  - the metadata records are [u32 4][ENDT], size field first;
  - the HASH tags do NOT match the zero-filled bitcode bodies, so
    scan.py reports HASH MISMATCH for both functions. That is correct
    behaviour for this incomplete reconstruction, not a parser bug:
    the real file's bitcode is not in the printed dump.

Usage: python3 rebuild_2015.py <out.metallib>
Stdlib only.
"""

import sys

FILE_SIZE = 0x1532

# The printed rows, 0x00-0x19f, copied verbatim from the article.
ROWS = """\
4d 54 4c 42 01 00 02 00 02 00 00 00 00 00 00 00
32 15 00 00 00 00 00 00 58 00 00 00 00 00 00 00
06 01 00 00 00 00 00 00 62 01 00 00 00 00 00 00
10 00 00 00 00 00 00 00 72 01 00 00 00 00 00 00
10 00 00 00 00 00 00 00 82 01 00 00 00 00 00 00
b0 13 00 00 00 00 00 00 02 00 00 00 82 00 00 00
4e 41 4d 45 0d 00 76 65 72 74 65 78 53 68 61 64
65 72 00 54 59 50 45 01 00 00 48 41 53 48 20 00
6d 1c 6e 48 df 84 fe 19 5a ad 33 01 96 29 15 20
ec fd 0e 31 08 a8 82 bd 39 de c3 69 cf ac b8 ff
4d 44 53 5a 08 00 f0 0a 00 00 00 00 00 00 4f 46
46 54 18 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 56 45 52 53
08 00 02 00 00 00 02 00 00 00 45 4e 44 54 84 00
00 00 4e 41 4d 45 0f 00 66 72 61 67 6d 65 6e 74
53 68 61 64 65 72 00 54 59 50 45 01 00 01 48 41
53 48 20 00 21 8a 2e 33 ea 7a 11 6b 76 97 bb 2d
b8 d0 5d ca 9d d8 67 57 68 b0 2c 24 05 c3 63 45
3e b6 cb 8c 4d 44 53 5a 08 00 c0 08 00 00 00 00
00 00 4f 46 46 54 18 00 08 00 00 00 00 00 00 00
08 00 00 00 00 00 00 00 f0 0a 00 00 00 00 00 00
56 45 52 53 08 00 02 00 00 00 02 00 00 00 45 4e
44 54 04 00 00 00 45 4e 44 54 04 00 00 00 45 4e
44 54 04 00 00 00 45 4e 44 54 04 00 00 00 45 4e
44 54 de c0 17 0b 00 00 00 00 14 00 00 00 dc 0a
00 00 ff ff ff ff 42 43 c0 de 35 14 00 00 03 00
"""


def main(argv):
    if len(argv) != 2:
        print("usage: rebuild_2015.py <out.metallib>", file=sys.stderr)
        return 2
    data = bytearray(FILE_SIZE)
    printed = bytes.fromhex("".join(ROWS.split()))
    assert len(printed) == 0x1A0, len(printed)
    data[0:len(printed)] = printed
    with open(argv[1], "wb") as f:
        f.write(data)
    print("wrote 2015 reconstruction (%d bytes) to %s" %
          (FILE_SIZE, argv[1]))
    print("note: bitcode bodies past 0x19f are zero-filled (not printed "
          "in the article); HASH tags will not match.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

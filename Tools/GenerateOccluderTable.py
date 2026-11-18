"""Writes Source/CORTAO/Private/CORTAOOccluderTable.inl from CORTAO's occluder lookup table.

    python Tools/GenerateOccluderTable.py <path to CORTAO>/tables/occluder/24x24x16x16_R16_UINT.bin

The table is 24 x 24 x 16 little-endian uint16 values for 16 rays (x fastest, then y, then z); the plugin
compiles it in and uploads it as a StructuredBuffer<uint>. Run this only when upstream changes the table.
CORTAO and its tables are by Daniel Isheden (MIT): https://github.com/theagentd/CORTAO
"""
import os
import struct
import sys

WIDTH, HEIGHT, DEPTH = 24, 24, 16


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    data = open(sys.argv[1], "rb").read()
    count = WIDTH * HEIGHT * DEPTH
    if len(data) != count * 2:
        raise SystemExit("expected %d bytes (24 x 24 x 16 uint16), got %d" % (count * 2, len(data)))
    values = struct.unpack("<%dH" % count, data)

    lines = [
        "// Generated from CORTAO tables/occluder/24x24x16x16_R16_UINT.bin (MIT, Daniel Isheden).",
        "// A 24 x 24 x 16 occluder lookup table for 16 rays, x fastest, then y, then z.",
        "// Regenerate by reading the .bin as little-endian uint16; do not edit by hand.",
        "",
        "static const uint16 GCORTAOOccluderTable16[24 * 24 * 16] =",
        "{",
    ]
    for row in range(0, count, WIDTH):
        lines.append("\t" + " ".join("0x%04x," % v for v in values[row:row + WIDTH]))
    lines.append("};")

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Source", "CORTAO", "Private", "CORTAOOccluderTable.inl")
    with open(out, "wb") as f:
        f.write(("\n".join(lines) + "\n").encode("ascii"))
    print("wrote %d values to %s" % (count, os.path.normpath(out)))


main()

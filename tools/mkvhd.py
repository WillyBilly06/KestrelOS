#!/usr/bin/env python3
"""mkvhd.py - wrap a raw disk image in a fixed-size VHD footer.

Only used for testing: Windows can mount a VHD directly, which is the quickest
way to confirm that the GPT and the FAT volumes this build produces are
readable by an implementation that is not our own.

    python mkvhd.py out/kestrelos.img test.vhd
"""
import os
import struct
import sys


def build(raw_path, vhd_path):
    size = os.path.getsize(raw_path)

    # Geometry must cover the image; the CHS fields are otherwise vestigial.
    total_sectors = size // 512
    if total_sectors > 65535 * 16 * 255:
        total_sectors = 65535 * 16 * 255

    if total_sectors >= 65535 * 16 * 63:
        spt, heads = 255, 16
        cylinders = total_sectors // spt // heads
    else:
        spt = 17
        cylinders = total_sectors // spt
        heads = max(4, (cylinders + 1023) // 1024)
        if heads > 16 or cylinders >= heads * 1024:
            spt = 31
            heads = 16
            cylinders = total_sectors // spt // heads
        if cylinders >= heads * 1024:
            spt = 63
            heads = 16
            cylinders = total_sectors // spt // heads
        cylinders = max(1, cylinders // heads if heads else cylinders)

    footer = bytearray(512)
    footer[0:8] = b"conectix"
    struct.pack_into(">I", footer, 8, 0x00000002)        # features: reserved bit
    struct.pack_into(">I", footer, 12, 0x00010000)       # file format version
    struct.pack_into(">Q", footer, 16, 0xFFFFFFFFFFFFFFFF)   # no dynamic header
    struct.pack_into(">I", footer, 24, 0)                # timestamp
    footer[28:32] = b"kstl"
    struct.pack_into(">I", footer, 32, 0x00010000)
    footer[36:40] = b"Wi2k"
    struct.pack_into(">Q", footer, 40, size)             # original size
    struct.pack_into(">Q", footer, 48, size)             # current size
    struct.pack_into(">H", footer, 56, min(cylinders, 65535))
    footer[58] = min(heads, 255)
    footer[59] = min(spt, 255)
    struct.pack_into(">I", footer, 60, 2)                # disk type: fixed
    footer[68:84] = bytes(range(16))                     # unique id
    footer[84] = 0                                       # saved state

    checksum = (~sum(footer)) & 0xFFFFFFFF
    struct.pack_into(">I", footer, 64, checksum)

    with open(raw_path, "rb") as src, open(vhd_path, "wb") as dst:
        while True:
            chunk = src.read(4 * 1024 * 1024)
            if not chunk:
                break
            dst.write(chunk)
        dst.write(footer)
    return vhd_path


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    build(sys.argv[1], sys.argv[2])
    print("wrote %s" % sys.argv[2])

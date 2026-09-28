#!/usr/bin/env python3
"""Relocate a raw image's backup GPT to the end of its destination disk.

A compact raw image necessarily describes the end of the image as the end of
the disk.  After that image is written onto a larger USB stick, Windows rejects
the GPT because the alternate header is no longer in the device's final sector.
This tool validates the image's primary GPT before changing anything, then
updates the protective MBR and both GPT headers for the real device size.
"""

import os
import struct
import sys
import zlib


SECTOR = 512
GPT_SIGNATURE = b"EFI PART"


def fail(message):
    raise SystemExit("expandgpt: " + message)


def header_crc_valid(header):
    size = struct.unpack_from("<I", header, 12)[0]
    if size < 92 or size > SECTOR:
        return False
    expected = struct.unpack_from("<I", header, 16)[0]
    checked = bytearray(header[:size])
    struct.pack_into("<I", checked, 16, 0)
    return (zlib.crc32(checked) & 0xFFFFFFFF) == expected


def finish_header(header):
    size = struct.unpack_from("<I", header, 12)[0]
    struct.pack_into("<I", header, 16, 0)
    struct.pack_into("<I", header, 16,
                     zlib.crc32(header[:size]) & 0xFFFFFFFF)


def read_exact(stream, offset, count):
    stream.seek(offset)
    data = stream.read(count)
    if len(data) != count:
        fail("the destination ended while its partition table was being read")
    return data


def expand(path, device_bytes):
    if device_bytes % SECTOR:
        fail("the destination size is not a whole number of 512-byte sectors")
    total_sectors = device_bytes // SECTOR
    if total_sectors < 68:
        fail("the destination is too small to hold a GPT")

    with open(path, "r+b", buffering=0) as disk:
        mbr = bytearray(read_exact(disk, 0, SECTOR))
        primary = bytearray(read_exact(disk, SECTOR, SECTOR))

        if mbr[510:512] != b"\x55\xaa" or mbr[450] != 0xEE:
            fail("the destination has no protective GPT MBR")
        if primary[:8] != GPT_SIGNATURE:
            fail("the destination has no primary GPT header")
        if not header_crc_valid(primary):
            fail("the primary GPT header CRC is invalid")
        if struct.unpack_from("<Q", primary, 24)[0] != 1:
            fail("the primary GPT is not in sector 1")

        entries_lba = struct.unpack_from("<Q", primary, 72)[0]
        entry_count = struct.unpack_from("<I", primary, 80)[0]
        entry_size = struct.unpack_from("<I", primary, 84)[0]
        table_bytes = entry_count * entry_size
        if entries_lba != 2 or entry_size < 128 or table_bytes <= 0 or table_bytes > 1024 * 1024:
            fail("the primary GPT uses an unsupported partition-entry layout")

        table_sectors = (table_bytes + SECTOR - 1) // SECTOR
        table = read_exact(disk, entries_lba * SECTOR,
                           table_sectors * SECTOR)
        expected_table_crc = struct.unpack_from("<I", primary, 88)[0]
        if (zlib.crc32(table[:table_bytes]) & 0xFFFFFFFF) != expected_table_crc:
            fail("the primary GPT partition-entry CRC is invalid")

        last_lba = total_sectors - 1
        backup_entries_lba = last_lba - table_sectors
        last_usable = backup_entries_lba - 1
        first_usable = struct.unpack_from("<Q", primary, 40)[0]
        if last_usable < first_usable:
            fail("the destination has no usable space between its GPT headers")

        # Refuse to silently make any existing partition invalid.  Empty GPT
        # entries have an all-zero type GUID.
        for index in range(entry_count):
            entry = table[index * entry_size:(index + 1) * entry_size]
            if entry[:16] == bytes(16):
                continue
            first = struct.unpack_from("<Q", entry, 32)[0]
            last = struct.unpack_from("<Q", entry, 40)[0]
            if first < first_usable or first > last or last > last_usable:
                fail("partition %d does not fit on the destination" % (index + 1))

        # The protective MBR must cover the actual device, not merely the
        # compact image that was copied onto it.
        struct.pack_into("<I", mbr, 458, min(last_lba, 0xFFFFFFFF))

        struct.pack_into("<Q", primary, 32, last_lba)
        struct.pack_into("<Q", primary, 48, last_usable)
        finish_header(primary)

        backup = bytearray(primary)
        struct.pack_into("<Q", backup, 24, last_lba)
        struct.pack_into("<Q", backup, 32, 1)
        struct.pack_into("<Q", backup, 72, backup_entries_lba)
        finish_header(backup)

        disk.seek(0)
        disk.write(mbr)
        disk.seek(SECTOR)
        disk.write(primary)
        disk.seek(backup_entries_lba * SECTOR)
        disk.write(table)
        disk.seek(last_lba * SECTOR)
        disk.write(backup)
        disk.flush()
        os.fsync(disk.fileno())

        # Verify the four pieces from the destination itself.  This catches a
        # device or bridge that acknowledged a write it did not retain.
        if read_exact(disk, 0, SECTOR) != bytes(mbr):
            fail("the protective MBR did not read back correctly")
        if read_exact(disk, SECTOR, SECTOR) != bytes(primary):
            fail("the primary GPT did not read back correctly")
        if read_exact(disk, backup_entries_lba * SECTOR,
                      len(table)) != table:
            fail("the backup partition entries did not read back correctly")
        if read_exact(disk, last_lba * SECTOR, SECTOR) != bytes(backup):
            fail("the backup GPT did not read back correctly")

    print("GPT expanded and verified: backup header moved to sector %d" % last_lba)


def main():
    if len(sys.argv) != 3:
        fail("usage: expandgpt.py DISK DEVICE_BYTES")
    try:
        device_bytes = int(sys.argv[2], 0)
    except ValueError:
        fail("DEVICE_BYTES is not an integer")
    expand(sys.argv[1], device_bytes)


if __name__ == "__main__":
    main()

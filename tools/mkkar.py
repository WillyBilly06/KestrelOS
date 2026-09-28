#!/usr/bin/env python3
"""mkkar.py - build a KestrelOS initrd archive.

The format is deliberately trivial: a header, a table of fixed-size entries and
then the file data, all little-endian.  Entries are emitted parents-first so the
kernel can create directories as it walks the table.

    python mkkar.py out.kar /bin/shell=build/shell.elf /etc/motd=motd.txt
"""
import os
import struct
import sys

MAGIC = 0x3152414B          # "KAR1"
NAME_MAX = 100
ENTRY_SIZE = 128
HEADER_SIZE = 24

KAR_FILE = 1
KAR_DIR = 2


def _normalise(path):
    """Absolute, forward-slashed, no trailing slash, no empty components."""
    parts = [p for p in path.replace("\\", "/").split("/") if p and p != "."]
    return "/" + "/".join(parts)


def build(tree, out_path):
    """tree maps an in-image path to a host file path.  Returns the entry count."""
    files = {}
    for dest, src in tree.items():
        dest = _normalise(dest)
        if len(dest.encode("utf-8")) >= NAME_MAX:
            raise ValueError("path too long for the archive format: %s" % dest)
        if not os.path.isfile(src):
            raise FileNotFoundError(src)
        files[dest] = src

    # Every parent directory of every file, so the kernel never has to guess.
    dirs = set()
    for dest in files:
        parts = dest.split("/")[1:-1]
        for i in range(1, len(parts) + 1):
            dirs.add("/" + "/".join(parts[:i]))

    entries = []
    for d in sorted(dirs):                      # shallow before deep
        entries.append((d, KAR_DIR, 0o755, None))
    for f in sorted(files):
        entries.append((f, KAR_FILE, 0o755 if "/bin/" in f else 0o644, files[f]))

    entry_offset = HEADER_SIZE
    data_offset = entry_offset + len(entries) * ENTRY_SIZE
    # Page-align the first file so the kernel can point straight at the image.
    data_offset = (data_offset + 0xFFF) & ~0xFFF

    blobs = []
    cursor = data_offset
    table = bytearray()

    for name, kind, mode, src in entries:
        raw = name.encode("utf-8")
        if kind == KAR_DIR:
            size, offset = 0, 0
        else:
            with open(src, "rb") as fh:
                blob = fh.read()
            size = len(blob)
            offset = cursor
            blobs.append((offset, blob))
            cursor += size
            cursor = (cursor + 15) & ~15        # keep entries tidily aligned

        # The C struct is name[100], u32 type, u32 mode, u64 size, u64 offset.
        # `size` is 8-byte aligned, so the compiler inserts four bytes of
        # padding after `mode`; the "4x" reproduces exactly that.
        entry = raw.ljust(NAME_MAX, b"\0") + struct.pack("<II4xQQ", kind, mode, size, offset)
        if len(entry) != ENTRY_SIZE:
            raise AssertionError("entry is %d bytes, expected %d" % (len(entry), ENTRY_SIZE))
        table += entry

    total = cursor
    header = struct.pack("<IIQII", MAGIC, len(entries), total, entry_offset, data_offset)

    image = bytearray(total)
    image[0:len(header)] = header
    image[entry_offset:entry_offset + len(table)] = table
    for offset, blob in blobs:
        image[offset:offset + len(blob)] = blob

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as fh:
        fh.write(image)
    return len(entries)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    out = sys.argv[1]
    tree = {}
    for arg in sys.argv[2:]:
        if "=" not in arg:
            print("expected dest=source, got %r" % arg)
            return 1
        dest, src = arg.split("=", 1)
        tree[dest] = src
    n = build(tree, out)
    print("wrote %s (%d entries, %d bytes)" % (out, n, os.path.getsize(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

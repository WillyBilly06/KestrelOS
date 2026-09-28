#!/usr/bin/env python3
"""Preserve owned EDID through NVIDIA's existing validation path.

NVIDIA 595.99.02's nvDpyReadAndParseEdidEvo() copies a client override,
writes it to RM with SET_EDID_V2, frees the client copy, and then rereads it
with GET_EDID_V2 before parsing.  The KestrelOS Blackwell RM port can drive
the sink but the custom-EDID SET/GET cache round trip returns no bytes for the
target Acer DP-SST connector.  NVKMS consequently retains only 1024x768.

The override-specific branch goes directly to ValidateEdid/PatchAndParseEdid.
For live DP-SST EDID, retain the SET_EDID attempt and its temporary-buffer
cleanup, but preserve the DP-owned bytes rather than freeing them and relying
on a second RM read. Validation, checksum policy, parsing and mode validation
remain NVIDIA's. DP-MST already validates its owned bytes directly. Paths
without a successful DP read still use stock RM retrieval.

This is deliberately an exact, fail-closed patch against the supplied
595.99.02 relocatable object.  A different vendor object or instruction
sequence is rejected rather than modified at a guessed offset.
"""

from __future__ import annotations

import hashlib
import pathlib
import struct
import sys

from patch_nvkms_dsc import patch_dsc


SOURCE_SHA256 = "27edc5af2db22f1e6046fcf7fad30f5a087876e55f7f7ac811d886c26dc1cf77"
SECTION_NAME = ".text.nvDpyReadAndParseEdidEvo"
BRANCH_OFFSET = 0x93E
ORIGINAL = bytes.fromhex("e9 4c f8 ff ff")  # jump to RM SET/GET round trip
PATCHED = bytes.fromhex("e9 9d fa ff ff")   # jump to direct validation at +0x3e0

# At +0x248 the SET_EDID temporary has been freed, but pEdid still owns the
# live DP buffer. The four-byte load precedes a relocated nvInternalFree call
# at +0x24c; do NOT overwrite that call or its relocation. Short-jump through
# the unreachable six-byte alignment NOP at +0x26a to the existing length
# reload/validation at +0x3e0. No relocation overlaps either changed range.
LIVE_PRESERVE_OFFSET = 0x248
LIVE_PRESERVE_ORIGINAL = bytes.fromhex("48 8b 7d 00")
LIVE_PRESERVE_PATCHED = bytes.fromhex("eb 20 0f 0b")  # +0x26a; unreachable UD2
LIVE_BRIDGE_OFFSET = 0x26A
LIVE_BRIDGE_ORIGINAL = bytes.fromhex("66 0f 1f 44 00 00")
LIVE_BRIDGE_PATCHED = bytes.fromhex("e9 71 01 00 00 90")  # +0x3e0; padding NOP


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def elf64_section(data: bytes, wanted: str) -> tuple[int, int]:
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise SystemExit("input is not a little-endian ELF64 object")
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    if shentsize < 64 or shnum == 0 or shstrndx >= shnum:
        raise SystemExit("invalid ELF section table")

    def header(index: int) -> tuple[int, int, int]:
        off = shoff + index * shentsize
        if off + 64 > len(data):
            raise SystemExit("ELF section header lies outside the input")
        name = struct.unpack_from("<I", data, off)[0]
        file_off = struct.unpack_from("<Q", data, off + 0x18)[0]
        size = struct.unpack_from("<Q", data, off + 0x20)[0]
        return name, file_off, size

    _, names_off, names_size = header(shstrndx)
    names = data[names_off:names_off + names_size]
    if len(names) != names_size:
        raise SystemExit("ELF section-name table lies outside the input")

    for index in range(shnum):
        name_off, file_off, size = header(index)
        if name_off >= len(names):
            continue
        end = names.find(b"\0", name_off)
        if end < 0:
            continue
        name = names[name_off:end].decode("ascii", errors="strict")
        if name == wanted:
            if file_off + size > len(data):
                raise SystemExit(f"ELF section {wanted} lies outside the input")
            return file_off, size
    raise SystemExit(f"ELF section not found: {wanted}")


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} SOURCE.o DEST.o")
    source = pathlib.Path(sys.argv[1])
    dest = pathlib.Path(sys.argv[2])
    original = source.read_bytes()
    actual_hash = sha256(original)
    if actual_hash != SOURCE_SHA256:
        raise SystemExit(
            "refusing unknown NVKMS core: "
            f"expected SHA-256 {SOURCE_SHA256}, got {actual_hash}"
        )

    section_off, section_size = elf64_section(original, SECTION_NAME)
    patches = (
        (BRANCH_OFFSET, ORIGINAL, PATCHED),
        (LIVE_PRESERVE_OFFSET, LIVE_PRESERVE_ORIGINAL, LIVE_PRESERVE_PATCHED),
        (LIVE_BRIDGE_OFFSET, LIVE_BRIDGE_ORIGINAL, LIVE_BRIDGE_PATCHED),
    )
    for offset, before, after in patches:
        if len(before) != len(after) or offset + len(before) > section_size:
            raise SystemExit("patch site lies outside the NVKMS function section")
        found = original[section_off + offset:section_off + offset + len(before)]
        if found != before:
            raise SystemExit(f"unexpected bytes at +0x{offset:x}: expected {before.hex()}, got {found.hex()}")
    result = bytearray(original)
    for offset, before, after in patches:
        result[section_off + offset:section_off + offset + len(before)] = after
    result = patch_dsc(bytes(result))
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(result)
    print(
        f"NVKMS owned-EDID patch PASS: {SECTION_NAME} override +0x{BRANCH_OFFSET:x}, "
        f"live DP +0x{LIVE_PRESERVE_OFFSET:x}/+0x{LIVE_BRIDGE_OFFSET:x}; "
        f"DP DSC threshold corrected in C5/C9; output SHA-256 {sha256(result)}"
    )


if __name__ == "__main__":
    main()

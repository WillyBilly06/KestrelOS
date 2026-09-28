#!/usr/bin/env python3
"""Correct DP DSC flatness threshold in the exact 595.99.02 NVKMS object.

Input is the owned-EDID-preserving derivative, never the vendor original.
NVIDIA's PPS generator accepts 8/10/12 bits per component and packs that
value into pps[0][31:28]. The DP HAL incorrectly uses bitsPerPixelX16.
Both the C5 and C9 HALs inline the calculation in EvoSetDscParams.

Replace only the 22-byte DP calculation, preserving function size, branch
targets and relocations. The removed stack store is dead (there are no
reads from rsp+0xc in either function). RDI is scratch at this point; both
paths reload it before its next use. EAX, EDX and ESI remain unchanged for
the immediately following channel-ownership check. HDMI is untouched.
This corrects a proven arithmetic defect; it does NOT prove sink visibility.
"""
import hashlib
import io

from elftools.elf.elffile import ELFFile

INPUT_SHA256 = "ac87e58bd65dfd2f067a32f12551ab87663fec3761df83b2648cd8cd4a19135a"
PATCH_OFFSET = 0x170
PATCHES = (
    (".text.EvoSetDscParamsC5",
     bytes.fromhex("48 8b 3c 24 41 bf 02 00 00 00 8b 3f 8d 4f f8 89 7c 24 0c 41 d3 e7"),
     bytes.fromhex("48 8b 3c 24 8b 4f 04 c1 e9 1c 83 e9 08 41 bf 02 00 00 00 41 d3 e7")),
    (".text.EvoSetDscParamsC9",
     bytes.fromhex("48 8b 3c 24 41 be 02 00 00 00 8b 3f 8d 4f f8 89 7c 24 0c 41 d3 e6"),
     bytes.fromhex("48 8b 3c 24 8b 4f 04 c1 e9 1c 83 e9 08 41 be 02 00 00 00 41 d3 e6")),
)


def patch_dsc(original: bytes) -> bytes:
    if hashlib.sha256(original).hexdigest() != INPUT_SHA256:
        raise ValueError("refusing unknown owned-EDID NVKMS object for DSC correction")
    elf = ELFFile(io.BytesIO(original))
    if elf.elfclass != 64 or not elf.little_endian or elf['e_type'] != 'ET_REL':
        raise ValueError("DSC correction requires a little-endian ELF64 relocatable object")
    result = bytearray(original)
    for name, before, after in PATCHES:
        section = elf.get_section_by_name(name)
        if section is None or len(before) != len(after):
            raise ValueError("missing DSC function or mismatched patch length")
        if section.data()[PATCH_OFFSET:PATCH_OFFSET + len(before)] != before:
            raise ValueError("unexpected DP DSC instruction sequence: " + name)
        index = elf.get_section_index(name)
        for relocs in elf.iter_sections():
            if relocs['sh_type'] not in ('SHT_RELA', 'SHT_REL') or relocs['sh_info'] != index:
                continue
            for relocation in relocs.iter_relocations():
                # Conservatively reserve eight bytes even for 32-bit relocations.
                off = relocation['r_offset']
                if off < PATCH_OFFSET + len(before) and off + 8 > PATCH_OFFSET:
                    raise ValueError("relocation overlaps DSC correction: " + name)
        off = section['sh_offset'] + PATCH_OFFSET
        result[off:off + len(before)] = after
    return bytes(result)

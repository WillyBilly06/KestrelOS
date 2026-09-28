"""Read-only, SHA-pinned ELF relocation audit for captured RC register records.

No driver loading or execution. Check the actual message-reference addends:
the symbols name descriptor arrays, not necessarily the selected message.
"""
from pathlib import Path
import hashlib
import struct

ROOT = Path(__file__).resolve().parents[1]
blob = (ROOT / 'Linux NVIDIA Driver/kernel/nvidia/nv-kernel.o_binary').read_bytes()
assert hashlib.sha256(blob).hexdigest() == 'b64589a3760bd2acea31ca104181ad98c1af3fdc588d251c2d1bdb55a8ccdc54'
assert blob[:6] == b'\x7fELF\x02\x01'
shoff = struct.unpack_from('<Q', blob, 40)[0]
entsize, count = struct.unpack_from('<HH', blob, 58)
sections = [struct.unpack_from('<IIQQQQIIQQ', blob, shoff+i*entsize)
            for i in range(count)]
shstr = sections[struct.unpack_from('<H', blob, 62)[0]]


def section_name(section):
    start = shstr[4] + section[0]
    return blob[start:blob.index(b'\0', start)].decode()


symsec = next(s for s in sections if s[1] == 2)
strings = sections[symsec[6]]
symbols = []
for off in range(symsec[4], symsec[4]+symsec[5], symsec[9]):
    name, info, other, section, value, size = struct.unpack_from('<IBBHQQ', blob, off)
    start = strings[4]+name
    symbol_name = blob[start:blob.index(b'\0', start)].decode()
    if not symbol_name and info & 15 == 3:  # ELF STT_SECTION
        symbol_name = section_name(sections[section])
    symbols.append((symbol_name, section, value, size))
by_name = {s[0]: s for s in symbols}
relocations = {}
for sec in sections:
    if sec[1] != 4:
        continue
    for off in range(sec[4], sec[4]+sec[5], sec[9]):
        address, info, addend = struct.unpack_from('<QQq', blob, off)
        relocations[sec[7], address] = (symbols[info >> 32][0], addend, info & 0xffffffff)


def data(name, delta=0):
    _, section, value, size = by_name[name]
    assert 0 <= delta < size
    return sections[section][4]+value+delta


def reference(name, delta, target, addend=0, kind=1):
    _, section, value, _ = by_name[name]
    assert relocations[section, value+delta] == (target, addend, kind), (name, delta)


def field_table(name, expected):
    assert by_name[name][3] == 24*len(expected)
    actual = [struct.unpack_from('<II', blob, data(name, i*24))
              for i in range(len(expected))]
    assert actual == expected, (name, actual)


# Dcl tag 302 -> message index 2 in the six-entry array, not its first message.
dcl = '_nv049163rm'
entry = next(i for i in range(by_name[dcl][3]//24)
             if struct.unpack_from('<I', blob, data(dcl, 24*i))[0] == 302)
assert struct.unpack_from('<I', blob, data(dcl, 24*entry+4))[0] == 65
reference(dcl, 24*entry+8, '_nv049319rm', 32)
assert struct.unpack_from('<Q', blob, data('_nv049319rm', 32))[0] == 5
reference('_nv049319rm', 40, '_nv049186rm')
field_table('_nv049186rm', [(1, 65), (2, 66), (3, 66), (4, 66), (5, 66)])
reference('_nv049186rm', 4*24+8, '_nv049330rm')
assert struct.unpack_from('<Q', blob, data('_nv049330rm'))[0] == 4
reference('_nv049330rm', 8, '_nv049299rm')
# Required enum type, optional uint64 offset, defaulted uint32 stride,
# repeated packed uint32 values: matches generated g_regs_pb.c.
field_table('_nv049299rm', [(1, 52), (2, 21), (3, 273), (4, 530)])
print('PASS SHA-pinned Dcl302[5] -> Regs.RegsAndMem descriptor and relocation chain')

# The remaining captured records must not be interpreted as NVENC firmware
# errors. Tag 304's producer identifies its own journal payload as "PERF".
# Check both descriptor selection and the actual producer's relocation; a
# nearby string or an equal field count alone would not establish attribution.
for tag, addend in ((304, 0), (305, 16)):
    entry = next(i for i in range(by_name[dcl][3]//24)
                 if struct.unpack_from('<I', blob, data(dcl, 24*i))[0] == tag)
    reference(dcl, 24*entry+8, '_nv049327rm', addend)
assert struct.unpack_from('<Q', blob, data('_nv049327rm'))[0] == 20
assert struct.unpack_from('<Q', blob, data('_nv049327rm', 16))[0] == 26
reference('_nv049327rm', 8, '_nv049287rm')
reference('_nv049327rm', 24, '_nv049286rm')
# Producer requests Dcl field 304 (table entry 4) and labels its emitted block.
reference('_nv040693rm', 0x13, dcl, 0x60, kind=11)  # R_X86_64_32S
reference('_nv040693rm', 0x269, '.rodata', 0x4959f4f, kind=11)
rodata_section = next(s for s in sections if section_name(s) == '.rodata')
label = rodata_section[4] + 0x4959f4f
assert blob[label:label+5] == b'PERF\0'
print('PASS Dcl304 PERF producer/label and Dcl305 register-array descriptors; no NVENC cause inferred')

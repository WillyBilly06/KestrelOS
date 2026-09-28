#!/usr/bin/env python3
"""Verify the exact bytes in the dedicated GPU-test disk image.

This intentionally reads the finished GPT/FAT image instead of trusting the
inputs handed to mkimage.  It also opens the KAR found *inside* that FAT image
and hashes the embedded NVIDIA firmware, catching stale-image mistakes that
previously made it all the way to the USB stick.
"""
from pathlib import Path
import hashlib
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "out/kestrelos-gputest.img"
EXPECTED_FW = "f2848c5da315a636a4b03945b9edebb762eda2140df750ce2cf140b9d4b19fc3"
EXPECTED_ACER_EDID = "00f739b87e9de120b7f2ec0921ccaa90111bb2fd35ce317dd6056baf78429b87"
EXPECTED_CMDLINE = b"cmdline="
EXPECTED_MODE = b"displaymode=extend"
EXPECTED_DEFAULT = b"default=gputest"
ESP_GUID = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")
DEDICATED_REFERENCES = {
    "kernel": ROOT / "out/gputest-kernel.elf",
    "initrd": ROOT / "out/gputest-initrd.kar",
    "Kestrel UEFI loader": ROOT / "out/gputest-loader.efi",
}


def die(message):
    raise SystemExit("FAIL: " + message)


def u16(buf, off): return struct.unpack_from("<H", buf, off)[0]
def u32(buf, off): return struct.unpack_from("<I", buf, off)[0]
def u64(buf, off): return struct.unpack_from("<Q", buf, off)[0]


class FatReader:
    def __init__(self, volume):
        self.v = volume
        self.bps = u16(volume, 11)
        self.spc = volume[13]
        self.reserved = u16(volume, 14)
        self.nfats = volume[16]
        self.root_entries = u16(volume, 17)
        self.fat_sectors = u16(volume, 22) or u32(volume, 36)
        self.root_sectors = (self.root_entries * 32 + self.bps - 1) // self.bps
        self.data_sector = self.reserved + self.nfats * self.fat_sectors + self.root_sectors
        self.root_cluster = u32(volume, 44) if self.root_entries == 0 else 0
        if self.bps != 512 or self.spc == 0 or self.nfats == 0:
            die("invalid ESP FAT BPB")
        self.fat32 = self.root_entries == 0

    def cluster(self, number):
        off = (self.data_sector + (number - 2) * self.spc) * self.bps
        return self.v[off:off + self.spc * self.bps]

    def next_cluster(self, number):
        fat = self.reserved * self.bps
        if self.fat32:
            return u32(self.v, fat + number * 4) & 0x0fffffff
        return u16(self.v, fat + number * 2)

    def chain(self, first):
        seen = set()
        while first >= 2 and first not in seen:
            seen.add(first)
            yield first
            nxt = self.next_cluster(first)
            if nxt >= (0x0ffffff8 if self.fat32 else 0xfff8):
                return
            first = nxt
        die("invalid or cyclic FAT chain")

    def directory_bytes(self, first):
        if first:
            return b"".join(self.cluster(c) for c in self.chain(first))
        start = (self.reserved + self.nfats * self.fat_sectors) * self.bps
        return self.v[start:start + self.root_sectors * self.bps]

    def entries(self, first):
        result = {}
        lfn = {}
        for off in range(0, len(data := self.directory_bytes(first)), 32):
            ent = data[off:off + 32]
            if len(ent) < 32 or ent[0] == 0: break
            if ent[0] == 0xe5:
                lfn = {}
                continue
            if ent[11] == 0x0f:
                chars = []
                for pos, count in ((1, 5), (14, 6), (28, 2)):
                    chars.extend(u16(ent, pos + i * 2) for i in range(count))
                lfn[ent[0] & 0x1f] = chars
                continue
            if ent[11] & 0x08:
                lfn = {}
                continue
            base = ent[0:8].decode("ascii", "replace").rstrip()
            ext = ent[8:11].decode("ascii", "replace").rstrip()
            name = base + (("." + ext) if ext else "")
            if lfn:
                codepoints = [c for n in sorted(lfn) for c in lfn[n]]
                codepoints = codepoints[:codepoints.index(0)] if 0 in codepoints else codepoints
                name = "".join(chr(c) for c in codepoints if c != 0xffff)
                lfn = {}
            cluster = u16(ent, 26) | ((u16(ent, 20) << 16) if self.fat32 else 0)
            result[name.upper()] = (ent[11], cluster, u32(ent, 28))
        return result

    def read(self, path):
        current = self.root_cluster
        parts = [p.upper() for p in path.replace("\\", "/").split("/") if p]
        for i, part in enumerate(parts):
            ent = self.entries(current).get(part)
            if not ent: die(f"ESP file missing: {path}")
            attr, cluster, size = ent
            if i != len(parts) - 1:
                if not (attr & 0x10): die(f"ESP path component is not a directory: {part}")
                current = cluster
            else:
                if attr & 0x10: die(f"ESP path is unexpectedly a directory: {path}")
                return b"".join(self.cluster(c) for c in self.chain(cluster))[:size]
        die(f"empty ESP path: {path}")


def kar_file(archive, wanted):
    if len(archive) < 24 or u32(archive, 0) != 0x3152414b:
        die("embedded INITRD.KAR has invalid header")
    count, total, table = u32(archive, 4), u64(archive, 8), u32(archive, 16)
    if total != len(archive): die("embedded INITRD.KAR length disagrees with header")
    for i in range(count):
        off = table + i * 128
        ent = archive[off:off + 128]
        if len(ent) != 128: die("truncated KAR table")
        name = ent[:100].split(b"\0", 1)[0].decode("utf-8")
        kind = u32(ent, 100)
        size, data_off = u64(ent, 112), u64(ent, 120)
        if name == wanted and kind == 1:
            if data_off + size > len(archive): die("KAR file lies outside archive")
            return archive[data_off:data_off + size]
    die(f"KAR file missing: {wanted}")


raw = IMAGE.read_bytes()
if len(raw) % 512 or raw[510:512] != b"\x55\xaa": die("invalid protective MBR")
if raw[512:520] != b"EFI PART": die("missing primary GPT header")
header_size = u32(raw, 512 + 12)
header = bytearray(raw[512:512 + header_size])
stored_header_crc = u32(header, 16)
struct.pack_into("<I", header, 16, 0)
if zlib.crc32(header) & 0xffffffff != stored_header_crc: die("GPT header CRC mismatch")
entry_lba, entry_count, entry_size = u64(raw, 512 + 72), u32(raw, 512 + 80), u32(raw, 512 + 84)
table = raw[entry_lba * 512:entry_lba * 512 + entry_count * entry_size]
if zlib.crc32(table) & 0xffffffff != u32(raw, 512 + 88): die("GPT table CRC mismatch")
esp = None
for i in range(entry_count):
    ent = table[i * entry_size:(i + 1) * entry_size]
    if ent[:16] == ESP_GUID:
        first, last = u64(ent, 32), u64(ent, 40)
        esp = raw[first * 512:(last + 1) * 512]
        break
if esp is None: die("EFI System Partition not found")

fat = FatReader(esp)
cfg = fat.read("/KESTREL/BOOT.CFG")
kernel = fat.read("/KESTREL/KERNEL.ELF")
initrd = fat.read("/KESTREL/INITRD.KAR")
loader = fat.read("/EFI/BOOT/BOOTX64.EFI")
second_stage = fat.read("/EFI/BOOT/GRUBX64.EFI")
cfg_lines = set(cfg.replace(b"\r", b"").split(b"\n"))
if EXPECTED_CMDLINE not in cfg_lines or EXPECTED_MODE not in cfg_lines or EXPECTED_DEFAULT not in cfg_lines:
    die("embedded BOOT.CFG is not the dedicated extend-mode GPU test")
dedicated_present = [path.exists() for path in DEDICATED_REFERENCES.values()]
if any(dedicated_present) and not all(dedicated_present):
    die("dedicated GPU-test reference artifact set is incomplete")
use_dedicated = all(dedicated_present)
for label, embedded, source in (
    ("kernel", kernel, DEDICATED_REFERENCES["kernel"] if use_dedicated else ROOT / "build/kernel/kernel.elf"),
    ("initrd", initrd, DEDICATED_REFERENCES["initrd"] if use_dedicated else ROOT / "build/initrd.kar"),
    ("signed UEFI shim", loader, ROOT / "secureboot/shimx64.signed.efi"),
    ("Kestrel UEFI loader", second_stage, DEDICATED_REFERENCES["Kestrel UEFI loader"] if use_dedicated else ROOT / "build/boot/BOOTX64.EFI"),
):
    if not source.exists(): die(f"missing build-side {label}: {source}")
    if hashlib.sha256(embedded).digest() != hashlib.sha256(source.read_bytes()).digest():
        die(f"embedded {label} differs from {'dedicated GPU-test reference' if use_dedicated and label != 'signed UEFI shim' else 'final build output'}")

fw = kar_file(initrd, "/lib/firmware/nvidia/gb202/gsp/gsp-595.99.02.bin")
fw_hash = hashlib.sha256(fw).hexdigest()
if fw_hash != EXPECTED_FW: die(f"embedded GSP firmware hash is {fw_hash}")
acer_edid = kar_file(initrd, "/lib/firmware/edid/nvkms-00000200.bin")
bindings = kar_file(initrd, "/lib/firmware/edid/nvkms-bindings.txt")
if bindings != (ROOT / "firmware/edid/nvkms-bindings.txt").read_bytes():
    die("embedded EDID port bindings do not match the configured cable layout")
acer_hash = hashlib.sha256(acer_edid).hexdigest()
if len(acer_edid) != 384 or acer_hash != EXPECTED_ACER_EDID:
    die(f"embedded connector-scoped Acer EDID is {len(acer_edid)} bytes, SHA-256 {acer_hash}")
if acer_edid[:8] != b"\x00\xff\xff\xff\xff\xff\xff\x00" or acer_edid[126] != 2:
    die("embedded Acer EDID has invalid header or extension count")
if acer_edid[75] != 0xFD or acer_edid[76] != 0x0C:
    die("embedded Acer EDID lacks the NVIDIA-compatible 48-240 Hz range repair")
for block in range(3):
    if sum(acer_edid[block * 128:(block + 1) * 128]) & 0xff:
        die(f"embedded Acer EDID block {block} checksum is nonzero")

print(f"PASS: GPT header/table CRCs and {len(raw)}-byte disk geometry")
print("PASS: embedded BOOT.CFG defaults to the named GPU-test entry in extend mode; main stays clean")
print("PASS: embedded UEFI loader, kernel and initrd exactly match " +
      ("dedicated GPU-test references" if use_dedicated else "final build outputs"))
print(f"PASS: embedded 595.99.02 GB202 GSP firmware {len(fw)} bytes, SHA-256 {fw_hash}")
print(f"PASS: embedded connector-scoped Acer X27U EDID {len(acer_edid)} bytes, SHA-256 {acer_hash}")

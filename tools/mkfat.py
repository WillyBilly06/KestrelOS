#!/usr/bin/env python3
"""mkfat.py - create a FAT16/FAT32 volume image and put files into it.

Written from the Microsoft FAT specification so the build does not need mkfs or
mtools.  Geometry is chosen the way the specification requires: the cluster
count alone decides the FAT width, so the code picks a cluster size that lands
the count safely inside the target type's range rather than near a boundary.
"""
import os
import struct
import sys

SECTOR = 512

FAT12_MAX = 4084          # a volume with this many clusters or fewer is FAT12
FAT16_MAX = 65524         # ... FAT16 up to here, FAT32 above
FAT32_MIN = 65525

# The cluster count alone decides the FAT width, and implementations differ by a
# cluster or two in how they compute it.  Landing near a boundary risks a driver
# reading the volume as the wrong type, so keep well clear of both edges.
FAT12_SAFE_MAX = 4000
FAT16_SAFE_MIN = 4200
FAT16_SAFE_MAX = 64000
FAT32_SAFE_MIN = 68000

ATTR_READ_ONLY = 0x01
ATTR_HIDDEN = 0x02
ATTR_SYSTEM = 0x04
ATTR_VOLUME_ID = 0x08
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20
ATTR_LFN = 0x0F


def _lfn_checksum(short_name):
    total = 0
    for c in short_name:
        total = (((total & 1) << 7) + (total >> 1) + c) & 0xFF
    return total


def _fits_short(name):
    if name in (".", ".."):
        return True
    dot = name.rfind(".")
    if dot == 0:
        return False
    base = name if dot < 0 else name[:dot]
    ext = "" if dot < 0 else name[dot + 1:]
    if not base or len(base) > 8 or len(ext) > 3:
        return False
    valid = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~`!(){}^#&")
    for c in base + ext:
        if c not in valid:
            return False
    return True


class FatVolume:
    """A FAT volume held in memory, written out in one go at the end."""

    def __init__(self, size_bytes, label="KESTREL", fat_type=None, oem="KESTREL "):
        self.total_sectors = size_bytes // SECTOR
        if self.total_sectors < 128:
            raise ValueError("volume too small: %d bytes" % size_bytes)

        self.label = (label.upper() + " " * 11)[:11]
        self.oem = (oem + " " * 8)[:8]
        self.reserved = 32          # FAT32 convention; trimmed below for FAT16
        self.num_fats = 2
        self.root_entries = 0

        self._choose_geometry(fat_type)
        self._build_layout()

        # cluster number -> next, EOC (0x0FFFFFFF) or 0 for free
        self.fat = [0] * (self.cluster_count + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF

        self.clusters = {}          # cluster number -> bytes (exactly one cluster)
        self.next_free = 2

        if self.fat_type == 32:
            self.root_cluster = self._alloc_chain(1)[0]
            self.root_dir = _Directory(self, self.root_cluster)
        else:
            self.root_cluster = 0
            self.root_dir = _Directory(self, None)   # the fixed root area

        self.root_dir.add_volume_label(self.label)

    # -------------------------------------------------------------- geometry

    def _choose_geometry(self, forced):
        """Pick a cluster size that yields a valid count for the chosen type."""
        candidates = []
        for spc in (1, 2, 4, 8, 16, 32, 64, 128):
            if spc * SECTOR > 32768:
                break
            candidates.append(spc)

        def try_type(fat_type):
            # Prefer the largest cluster still inside the type's range: fewer
            # clusters means a smaller FAT and less metadata churn.
            best = None
            for spc in candidates:
                data_sectors, count, fat_sectors, reserved = self._estimate(spc, fat_type)
                if count < 1:
                    continue
                if fat_type == 32 and not (FAT32_SAFE_MIN <= count <= 268435444):
                    continue
                if fat_type == 16 and not (FAT16_SAFE_MIN <= count <= FAT16_SAFE_MAX):
                    continue
                if fat_type == 12 and count > FAT12_SAFE_MAX:
                    continue
                best = (spc, count, fat_sectors, reserved)
            return best

        order = [forced] if forced else [32, 16, 12]
        for fat_type in order:
            picked = try_type(fat_type)
            if picked:
                self.fat_type = fat_type
                self.sectors_per_cluster, self.cluster_count, self.fat_sectors, self.reserved = picked
                return
        raise ValueError("no valid FAT geometry for %d sectors" % self.total_sectors)

    def _estimate(self, spc, fat_type):
        """Solve for the cluster count, given that the FAT size depends on it."""
        reserved = 32 if fat_type == 32 else 1
        root_entries = 0 if fat_type == 32 else 512
        root_sectors = (root_entries * 32 + SECTOR - 1) // SECTOR
        bits = {12: 12, 16: 16, 32: 32}[fat_type]

        # Start from an upper bound and iterate; it converges in a few rounds.
        fat_sectors = 1
        for _ in range(64):
            data_sectors = self.total_sectors - reserved - root_sectors - self.num_fats * fat_sectors
            if data_sectors <= 0:
                return 0, 0, 0, reserved
            count = data_sectors // spc
            need_bytes = ((count + 2) * bits + 7) // 8
            need = (need_bytes + SECTOR - 1) // SECTOR
            if need == fat_sectors:
                break
            fat_sectors = need
        data_sectors = self.total_sectors - reserved - root_sectors - self.num_fats * fat_sectors
        count = max(0, data_sectors // spc)
        return data_sectors, count, fat_sectors, reserved

    def _build_layout(self):
        self.root_entries = 0 if self.fat_type == 32 else 512
        self.root_sectors = (self.root_entries * 32 + SECTOR - 1) // SECTOR
        self.fat_start = self.reserved
        self.root_start = self.fat_start + self.num_fats * self.fat_sectors
        self.data_start = self.root_start + self.root_sectors
        self.bytes_per_cluster = self.sectors_per_cluster * SECTOR

    # ------------------------------------------------------------ allocation

    def _alloc_chain(self, count):
        chain = []
        for _ in range(count):
            while self.next_free < len(self.fat) and self.fat[self.next_free] != 0:
                self.next_free += 1
            if self.next_free >= len(self.fat):
                raise ValueError("the volume is full")
            chain.append(self.next_free)
            self.fat[self.next_free] = 0x0FFFFFFF
            self.clusters[self.next_free] = bytearray(self.bytes_per_cluster)
            self.next_free += 1
        for i in range(len(chain) - 1):
            self.fat[chain[i]] = chain[i + 1]
        return chain

    def free_clusters(self):
        return sum(1 for i in range(2, len(self.fat)) if self.fat[i] == 0)

    # ----------------------------------------------------------------- files

    def add_file(self, path, data):
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        if not parts:
            raise ValueError("empty path")
        directory = self.root_dir
        for part in parts[:-1]:
            directory = directory.ensure_dir(part)
        directory.add_file(parts[-1], data)

    def add_host_file(self, path, host_path):
        with open(host_path, "rb") as fh:
            self.add_file(path, fh.read())

    def ensure_dir(self, path):
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        directory = self.root_dir
        for part in parts:
            directory = directory.ensure_dir(part)
        return directory

    # ----------------------------------------------------------------- output

    def _boot_sector(self):
        b = bytearray(SECTOR)
        b[0:3] = b"\xEB\x58\x90" if self.fat_type == 32 else b"\xEB\x3C\x90"
        b[3:11] = self.oem.encode("ascii")

        struct.pack_into("<H", b, 11, SECTOR)
        b[13] = self.sectors_per_cluster
        struct.pack_into("<H", b, 14, self.reserved)
        b[16] = self.num_fats
        struct.pack_into("<H", b, 17, self.root_entries)
        struct.pack_into("<H", b, 19, 0 if self.total_sectors > 0xFFFF else self.total_sectors)
        b[21] = 0xF8                                  # fixed disk
        struct.pack_into("<H", b, 22, 0 if self.fat_type == 32 else self.fat_sectors)
        struct.pack_into("<H", b, 24, 63)             # sectors per track
        struct.pack_into("<H", b, 26, 255)            # heads
        struct.pack_into("<I", b, 28, 0)              # hidden sectors
        struct.pack_into("<I", b, 32, self.total_sectors if self.total_sectors > 0xFFFF else 0)

        if self.fat_type == 32:
            struct.pack_into("<I", b, 36, self.fat_sectors)
            struct.pack_into("<H", b, 40, 0)          # flags: mirror all FATs
            struct.pack_into("<H", b, 42, 0)          # version
            struct.pack_into("<I", b, 44, self.root_cluster)
            struct.pack_into("<H", b, 48, 1)          # FSInfo sector
            struct.pack_into("<H", b, 50, 6)          # backup boot sector
            b[64] = 0x80
            b[66] = 0x29                              # extended boot signature
            struct.pack_into("<I", b, 67, 0x4B455354) # volume id
            b[71:82] = self.label.encode("ascii")
            b[82:90] = b"FAT32   "
        else:
            b[36] = 0x80
            b[38] = 0x29
            struct.pack_into("<I", b, 39, 0x4B455354)
            b[43:54] = self.label.encode("ascii")
            b[54:62] = (b"FAT16   " if self.fat_type == 16 else b"FAT12   ")

        b[510] = 0x55
        b[511] = 0xAA
        return b

    def _fsinfo(self):
        b = bytearray(SECTOR)
        struct.pack_into("<I", b, 0, 0x41615252)
        struct.pack_into("<I", b, 484, 0x61417272)
        struct.pack_into("<I", b, 488, self.free_clusters())
        struct.pack_into("<I", b, 492, self.next_free)
        b[510] = 0x55
        b[511] = 0xAA
        return b

    def _pack_fat(self):
        raw = bytearray(self.fat_sectors * SECTOR)
        if self.fat_type == 32:
            for i, v in enumerate(self.fat):
                if 4 * i + 4 <= len(raw):
                    struct.pack_into("<I", raw, 4 * i, v & 0x0FFFFFFF)
        elif self.fat_type == 16:
            for i, v in enumerate(self.fat):
                if 2 * i + 2 <= len(raw):
                    struct.pack_into("<H", raw, 2 * i, 0xFFFF if v >= 0x0FFFFFF8 else (v & 0xFFFF))
        else:
            for i, v in enumerate(self.fat):
                value = 0xFFF if v >= 0x0FFFFFF8 else (v & 0xFFF)
                offset = i + (i // 2)
                if offset + 2 > len(raw):
                    break
                pair = raw[offset] | (raw[offset + 1] << 8)
                if i & 1:
                    pair = (pair & 0x000F) | (value << 4)
                else:
                    pair = (pair & 0xF000) | value
                raw[offset] = pair & 0xFF
                raw[offset + 1] = (pair >> 8) & 0xFF
        return raw

    def to_bytes(self):
        image = bytearray(self.total_sectors * SECTOR)

        boot = self._boot_sector()
        image[0:SECTOR] = boot
        if self.fat_type == 32:
            image[SECTOR:2 * SECTOR] = self._fsinfo()
            image[6 * SECTOR:7 * SECTOR] = boot
            image[7 * SECTOR:8 * SECTOR] = self._fsinfo()

        fat = self._pack_fat()
        for i in range(self.num_fats):
            at = (self.fat_start + i * self.fat_sectors) * SECTOR
            image[at:at + len(fat)] = fat

        if self.fat_type != 32:
            data = self.root_dir.serialise_fixed_root(self.root_entries)
            at = self.root_start * SECTOR
            image[at:at + len(data)] = data

        for cluster, content in self.clusters.items():
            at = (self.data_start + (cluster - 2) * self.sectors_per_cluster) * SECTOR
            image[at:at + len(content)] = content

        return bytes(image)

    def write(self, path):
        with open(path, "wb") as fh:
            fh.write(self.to_bytes())

    def describe(self):
        return "FAT%d, %d clusters of %d bytes, %d MiB" % (
            self.fat_type, self.cluster_count, self.bytes_per_cluster,
            self.total_sectors * SECTOR // (1024 * 1024))


class _Directory:
    """A directory being assembled.  Entries are flushed into clusters (or the
    fixed root area) as they are added."""

    def __init__(self, volume, first_cluster, parent_cluster=0):
        self.vol = volume
        self.first_cluster = first_cluster       # None means the FAT16 root
        self.parent_cluster = parent_cluster
        self.entries = bytearray()
        self.children = {}
        self.short_names = set()

        if first_cluster is not None and parent_cluster is not None and first_cluster != volume.root_cluster:
            self._append(self._short_entry(b".          ", ATTR_DIRECTORY, first_cluster, 0))
            self._append(self._short_entry(b"..         ", ATTR_DIRECTORY, parent_cluster, 0))

    # ------------------------------------------------------------- internals

    def _append(self, raw):
        self.entries += raw
        self._flush()

    def _flush(self):
        """Copy the entry bytes into the volume, growing the chain as needed."""
        if self.first_cluster is None:
            return                                   # written at the very end
        need = len(self.entries)
        chain = self._chain()
        have = len(chain) * self.vol.bytes_per_cluster
        while have < need:
            extra = self.vol._alloc_chain(1)[0]
            self.vol.fat[chain[-1]] = extra
            self.vol.fat[extra] = 0x0FFFFFFF
            chain.append(extra)
            have += self.vol.bytes_per_cluster

        for i, cluster in enumerate(chain):
            start = i * self.vol.bytes_per_cluster
            chunk = self.entries[start:start + self.vol.bytes_per_cluster]
            buf = self.vol.clusters[cluster]
            buf[0:len(chunk)] = chunk
            for j in range(len(chunk), self.vol.bytes_per_cluster):
                buf[j] = 0

    def _chain(self):
        chain = [self.first_cluster]
        while True:
            nxt = self.vol.fat[chain[-1]]
            if nxt >= 0x0FFFFFF8 or nxt == 0:
                break
            chain.append(nxt)
        return chain

    def _short_entry(self, name11, attr, cluster, size):
        e = bytearray(32)
        e[0:11] = name11
        e[11] = attr
        struct.pack_into("<H", e, 14, 0x8000)         # create time
        struct.pack_into("<H", e, 16, 0x5821)         # create date (2024-01-01)
        struct.pack_into("<H", e, 18, 0x5821)         # access date
        struct.pack_into("<H", e, 20, (cluster >> 16) & 0xFFFF)
        struct.pack_into("<H", e, 22, 0x8000)         # write time
        struct.pack_into("<H", e, 24, 0x5821)         # write date
        struct.pack_into("<H", e, 26, cluster & 0xFFFF)
        struct.pack_into("<I", e, 28, size)
        return e

    def _make_short(self, name):
        upper = name.upper()
        dot = upper.rfind(".")
        base = upper if dot <= 0 else upper[:dot]
        ext = "" if dot <= 0 else upper[dot + 1:]

        valid = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~`!(){}^#&")
        base = "".join(c if c in valid else "_" for c in base if c not in " .")[:8] or "_"
        ext = "".join(c if c in valid else "_" for c in ext)[:3]

        candidate = (base.ljust(8) + ext.ljust(3)).encode("ascii")
        if _fits_short(name) and candidate not in self.short_names:
            self.short_names.add(candidate)
            return candidate, False

        for n in range(1, 1000):
            suffix = "~%d" % n
            stem = (base[:8 - len(suffix)] + suffix).ljust(8)
            candidate = (stem + ext.ljust(3)).encode("ascii")
            if candidate not in self.short_names:
                self.short_names.add(candidate)
                return candidate, True
        raise ValueError("cannot find a unique short name for %s" % name)

    def _lfn_entries(self, name, short_name):
        checksum = _lfn_checksum(short_name)
        chars = [ord(c) for c in name] + [0]
        while len(chars) % 13:
            chars.append(0xFFFF)
        count = len(chars) // 13

        out = bytearray()
        for i in range(count, 0, -1):               # last part is stored first
            e = bytearray(32)
            e[0] = i | (0x40 if i == count else 0)
            e[11] = ATTR_LFN
            e[12] = 0
            e[13] = checksum
            struct.pack_into("<H", e, 26, 0)
            part = chars[(i - 1) * 13:i * 13]
            for j in range(5):
                struct.pack_into("<H", e, 1 + j * 2, part[j])
            for j in range(6):
                struct.pack_into("<H", e, 14 + j * 2, part[5 + j])
            for j in range(2):
                struct.pack_into("<H", e, 28 + j * 2, part[11 + j])
            out += e
        return out

    def _add_entry(self, name, attr, cluster, size):
        short_name, forced = self._make_short(name)
        raw = bytearray()
        if forced or not _fits_short(name):
            raw += self._lfn_entries(name, short_name)
        raw += self._short_entry(short_name, attr, cluster, size)
        self._append(raw)

    # ---------------------------------------------------------------- public

    def add_volume_label(self, label):
        self._append(self._short_entry(label.encode("ascii"), ATTR_VOLUME_ID, 0, 0))

    def ensure_dir(self, name):
        key = name.upper()
        if key in self.children:
            return self.children[key]

        cluster = self.vol._alloc_chain(1)[0]
        parent = 0 if self.first_cluster in (None, self.vol.root_cluster) else self.first_cluster
        child = _Directory(self.vol, cluster, parent)
        self._add_entry(name, ATTR_DIRECTORY, cluster, 0)
        self.children[key] = child
        return child

    def add_file(self, name, data):
        size = len(data)
        if size:
            count = (size + self.vol.bytes_per_cluster - 1) // self.vol.bytes_per_cluster
            chain = self.vol._alloc_chain(count)
            for i, cluster in enumerate(chain):
                chunk = data[i * self.vol.bytes_per_cluster:(i + 1) * self.vol.bytes_per_cluster]
                buf = self.vol.clusters[cluster]
                buf[0:len(chunk)] = chunk
            first = chain[0]
        else:
            first = 0
        self._add_entry(name, ATTR_ARCHIVE, first, size)

    def serialise_fixed_root(self, root_entries):
        raw = bytes(self.entries)
        limit = root_entries * 32
        if len(raw) > limit:
            raise ValueError("too many entries for a fixed root directory")
        return raw + b"\0" * (limit - len(raw))


def build_volume(size_bytes, files, label="KESTREL", fat_type=None):
    """files maps an in-volume path to a host path or to bytes."""
    vol = FatVolume(size_bytes, label=label, fat_type=fat_type)
    for dest, src in sorted(files.items()):
        if isinstance(src, (bytes, bytearray)):
            vol.add_file(dest, bytes(src))
        else:
            vol.add_host_file(dest, src)
    return vol


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    out = sys.argv[1]
    size_mb = int(sys.argv[2])
    files = {}
    for arg in sys.argv[3:]:
        dest, src = arg.split("=", 1)
        files[dest] = src
    vol = build_volume(size_mb * 1024 * 1024, files)
    vol.write(out)
    print("wrote %s: %s" % (out, vol.describe()))
    return 0


if __name__ == "__main__":
    sys.exit(main())

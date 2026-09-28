#!/usr/bin/env python3
"""mkimage.py - assemble the bootable KestrelOS artifacts.

Three outputs:
  * a raw GPT disk image with an EFI system partition and a data partition
  * a VMware descriptor pointing at that image, so it can be attached as a disk
  * an ISO with an El Torito "no emulation" EFI boot entry carrying the same ESP

Everything is written from the specifications - GPT, ISO 9660, El Torito - so
the build needs nothing beyond Python.
"""
import os
import struct
import sys
import zlib

SECTOR = 512

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkfat

# GPT partition type GUIDs, in the mixed-endian form GPT stores.
TYPE_ESP = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
TYPE_KESTREL = "B34C1A7E-9D62-4E47-9C31-5A6F2E88D140"


def guid_to_bytes(text):
    """Pack the canonical text form the way GPT stores it: the first three
    fields little-endian, the last two big-endian."""
    parts = text.split("-")
    if len(parts) != 5:
        raise ValueError("bad GUID: %s" % text)
    return (
        struct.pack("<I", int(parts[0], 16))
        + struct.pack("<H", int(parts[1], 16))
        + struct.pack("<H", int(parts[2], 16))
        + bytes.fromhex(parts[3])
        + bytes.fromhex(parts[4])
    )


def bytes_to_guid(raw):
    a = struct.unpack_from("<I", raw, 0)[0]
    b = struct.unpack_from("<H", raw, 4)[0]
    c = struct.unpack_from("<H", raw, 6)[0]
    return "%08X-%04X-%04X-%s-%s" % (a, b, c, raw[8:10].hex().upper(), raw[10:16].hex().upper())


def make_guid(seed):
    """A deterministic version-4-shaped GUID, so rebuilds are reproducible and
    the boot configuration can name the data partition up front."""
    digest = bytearray(zlib.crc32(seed.encode()).to_bytes(4, "little") * 4)
    for i in range(16):
        digest[i] ^= (zlib.crc32((seed + str(i)).encode()) >> (i % 24)) & 0xFF
    digest[6] = (digest[6] & 0x0F) | 0x40        # version 4
    digest[8] = (digest[8] & 0x3F) | 0x80        # variant 1
    return bytes(digest)


# --------------------------------------------------------------------- GPT

def _protective_mbr(total_sectors):
    mbr = bytearray(SECTOR)
    # One partition of type 0xEE spanning the disk, so tools that only
    # understand MBR see the whole thing as claimed rather than as free space.
    entry = bytearray(16)
    entry[0] = 0x00                              # not bootable
    entry[1:4] = b"\x00\x02\x00"                 # CHS start, first sector
    entry[4] = 0xEE
    entry[5:8] = b"\xFF\xFF\xFF"                 # CHS end, saturated
    struct.pack_into("<I", entry, 8, 1)
    struct.pack_into("<I", entry, 12, min(total_sectors - 1, 0xFFFFFFFF))
    mbr[446:462] = entry
    mbr[510] = 0x55
    mbr[511] = 0xAA
    return mbr


def _gpt_entry(type_guid, part_guid, first_lba, last_lba, name):
    e = bytearray(128)
    e[0:16] = type_guid
    e[16:32] = part_guid
    struct.pack_into("<Q", e, 32, first_lba)
    struct.pack_into("<Q", e, 40, last_lba)
    struct.pack_into("<Q", e, 48, 0)             # attributes
    encoded = name.encode("utf-16-le")[:70]
    e[56:56 + len(encoded)] = encoded
    return e


def _gpt_header(current_lba, backup_lba, first_usable, last_usable,
                disk_guid, entries_lba, entries, entry_count=128):
    table = bytearray()
    for i in range(entry_count):
        table += entries[i] if i < len(entries) else bytes(128)

    h = bytearray(92)
    h[0:8] = b"EFI PART"
    struct.pack_into("<I", h, 8, 0x00010000)     # revision 1.0
    struct.pack_into("<I", h, 12, 92)            # header size
    struct.pack_into("<I", h, 16, 0)             # CRC, filled in below
    struct.pack_into("<I", h, 20, 0)             # reserved
    struct.pack_into("<Q", h, 24, current_lba)
    struct.pack_into("<Q", h, 32, backup_lba)
    struct.pack_into("<Q", h, 40, first_usable)
    struct.pack_into("<Q", h, 48, last_usable)
    h[56:72] = disk_guid
    struct.pack_into("<Q", h, 72, entries_lba)
    struct.pack_into("<I", h, 80, entry_count)
    struct.pack_into("<I", h, 84, 128)           # bytes per entry
    struct.pack_into("<I", h, 88, zlib.crc32(bytes(table)) & 0xFFFFFFFF)
    struct.pack_into("<I", h, 16, zlib.crc32(bytes(h)) & 0xFFFFFFFF)
    return bytes(h), bytes(table)


def build_disk(path, esp_files, esp_mb=64, data_mb=64, data_files=None,
               data_raw=None):
    """Write a GPT image with an ESP and a KestrelOS data partition.

    Returns a dict describing the layout, including the data partition's GUID so
    the boot configuration can name it.

    `data_raw` puts the bytes of an already-formatted volume in the data
    partition instead of building a FAT one.  That exists for one reason worth
    stating: the machine this system is carried on has an NTFS data partition,
    and nothing here can format NTFS.  Windows can, and did - so a volume it
    formatted is lifted out of a disk image and dropped in here, which makes
    the configuration that actually matters testable without asking anybody to
    run anything as an administrator.
    """
    align = 2048                                  # 1 MiB alignment
    entries_sectors = 32                          # 128 entries of 128 bytes

    esp_sectors = (esp_mb * 1024 * 1024) // SECTOR
    if data_raw is not None:
        data_sectors = (len(data_raw) + SECTOR - 1) // SECTOR
    else:
        data_sectors = (data_mb * 1024 * 1024) // SECTOR

    esp_start = align
    esp_end = esp_start + esp_sectors - 1
    data_start = ((esp_end + 1 + align - 1) // align) * align
    data_end = data_start + data_sectors - 1

    # 1 protective MBR + 1 header + entries at each end, then a little slack.
    total = data_end + 1 + entries_sectors + 1 + align
    total = ((total + align - 1) // align) * align

    disk_guid = make_guid("kestrel-disk")
    esp_guid = make_guid("kestrel-esp")
    data_guid = make_guid("kestrel-data")

    entries = [
        _gpt_entry(guid_to_bytes(TYPE_ESP), esp_guid, esp_start, esp_end, "EFI System Partition"),
        _gpt_entry(guid_to_bytes(TYPE_KESTREL), data_guid, data_start, data_end, "KestrelOS"),
    ]

    first_usable = 2 + entries_sectors
    last_usable = total - 1 - 1 - entries_sectors

    primary, table = _gpt_header(1, total - 1, first_usable, last_usable,
                                 disk_guid, 2, entries)
    backup, _ = _gpt_header(total - 1, 1, first_usable, last_usable,
                            disk_guid, total - 1 - entries_sectors, entries)

    image = bytearray(total * SECTOR)
    image[0:SECTOR] = _protective_mbr(total)
    image[SECTOR:SECTOR + len(primary)] = primary
    image[2 * SECTOR:2 * SECTOR + len(table)] = table

    backup_table_lba = total - 1 - entries_sectors
    image[backup_table_lba * SECTOR:backup_table_lba * SECTOR + len(table)] = table
    image[(total - 1) * SECTOR:(total - 1) * SECTOR + len(backup)] = backup

    # The loader needs to know which partition holds the persistent data, and
    # the only way to say so is to bake it into the configuration it reads.
    data_guid_text = bytes_to_guid(data_guid)
    esp_files = dict(esp_files)
    if "/KESTREL/BOOT.CFG" in esp_files:
        cfg_path = esp_files["/KESTREL/BOOT.CFG"]
        with open(cfg_path, "r", encoding="utf-8") as fh:
            cfg = fh.read()
        if "data=" not in cfg:
            cfg = cfg.rstrip("\n") + "\ndata=PARTUUID=%s\n" % data_guid_text
        esp_files["/KESTREL/BOOT.CFG"] = cfg.encode("utf-8")

    esp = mkfat.build_volume(esp_sectors * SECTOR, esp_files, label="ESP")
    image[esp_start * SECTOR:(esp_start + esp_sectors) * SECTOR] = esp.to_bytes()

    if data_raw is not None:
        image[data_start * SECTOR:data_start * SECTOR + len(data_raw)] = data_raw
    else:
        data = mkfat.build_volume(data_sectors * SECTOR, data_files or {}, label="KESTREL")
        data.ensure_dir("/logs")
        image[data_start * SECTOR:(data_start + data_sectors) * SECTOR] = data.to_bytes()

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(image)

    return {
        "total_sectors": total,
        "esp_start": esp_start,
        "esp_sectors": esp_sectors,
        "esp": esp.describe(),
        "data_start": data_start,
        "data_sectors": data_sectors,
        # A volume that was handed to us already formatted has no builder to
        # describe itself; saying what it is is the honest substitute.
        "data": (data.describe() if data_raw is None
                 else {"kind": "pre-formatted", "bytes": len(data_raw)}),
        "data_guid": data_guid_text,
        "esp_guid": bytes_to_guid(esp_guid),
        "disk_guid": bytes_to_guid(disk_guid),
    }


# -------------------------------------------------------------------- VMDK

def write_vmdk_descriptor(vmdk_path, raw_path):
    """A monolithicFlat descriptor beside the raw image, so VMware can attach
    the very same bytes that get written to a USB stick."""
    size = os.path.getsize(raw_path)
    sectors = size // SECTOR
    extent = os.path.basename(raw_path)
    cid = zlib.crc32(extent.encode()) & 0xFFFFFFFF

    # A plausible geometry; VMware only sanity-checks that it covers the extent.
    heads, spt = 255, 63
    cylinders = max(1, sectors // (heads * spt))

    text = (
        "# Disk DescriptorFile\n"
        "version=1\n"
        "encoding=\"UTF-8\"\n"
        "CID=%08x\n"
        "parentCID=ffffffff\n"
        "isNativeSnapshot=\"no\"\n"
        "createType=\"monolithicFlat\"\n"
        "\n"
        "# Extent description\n"
        "RW %d FLAT \"%s\" 0\n"
        "\n"
        "# The Disk Data Base\n"
        "#DDB\n"
        "\n"
        "ddb.adapterType = \"lsilogic\"\n"
        "ddb.geometry.cylinders = \"%d\"\n"
        "ddb.geometry.heads = \"%d\"\n"
        "ddb.geometry.sectors = \"%d\"\n"
        "ddb.virtualHWVersion = \"14\"\n"
    ) % (cid, sectors, extent, cylinders, heads, spt)

    with open(vmdk_path, "w", newline="\n") as fh:
        fh.write(text)


# ------------------------------------------------------------------ ISO9660

def _iso_both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def _iso_both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def _iso_datetime():
    # ISO 9660 "digits" form: YYYYMMDDHHMMSSss followed by a GMT offset.
    return b"2024010100000000" + bytes([0])


def _iso_dir_record(name, extent, length, is_dir, name_is_special=False):
    """One directory record.  `name` is already in the on-disk form."""
    raw_name = name if isinstance(name, bytes) else name.encode("ascii")
    n = len(raw_name)
    size = 33 + n + (1 if n % 2 == 0 else 0)

    r = bytearray(size)
    r[0] = size
    r[1] = 0                                     # extended attribute length
    r[2:10] = _iso_both32(extent)
    r[10:18] = _iso_both32(length)
    r[18:25] = bytes([124, 1, 1, 0, 0, 0, 0])    # 2024-01-01 00:00:00 UTC
    r[25] = 0x02 if is_dir else 0x00
    r[26] = 0                                    # file unit size
    r[27] = 0                                    # interleave gap
    r[28:32] = _iso_both16(1)                    # volume sequence number
    r[32] = n
    r[33:33 + n] = raw_name
    return bytes(r)


def build_iso(path, esp_files, esp_mb=64, volume_id="KESTRELOS"):
    """A UEFI-bootable ISO.

    The El Torito entry is a "no emulation" boot image holding a complete FAT
    volume; firmware exposes it as a filesystem and loads BOOTX64.EFI from it.
    The same files also appear in the ISO 9660 tree so the disc is readable on
    an ordinary computer.
    """
    BLOCK = 2048

    esp = mkfat.build_volume(esp_mb * 1024 * 1024, esp_files, label="ESP")
    esp_image = esp.to_bytes()

    # Layout, in 2048-byte blocks:
    #   0-15   system area
    #   16     primary volume descriptor
    #   17     El Torito boot record
    #   18     terminator
    #   19     boot catalog
    #   20     root directory
    #   21+    the ESP image, then the files
    pvd_lba = 16
    boot_record_lba = 17
    terminator_lba = 18
    catalog_lba = 19
    root_lba = 20

    esp_lba = 21
    esp_blocks = (len(esp_image) + BLOCK - 1) // BLOCK

    # Only the top-level entries need to be in the ISO tree; the FAT image is
    # what firmware actually boots from.
    flat = {}
    for dest, src in esp_files.items():
        leaf = dest.strip("/").split("/")[-1].upper()
        if isinstance(src, (bytes, bytearray)):
            flat[leaf] = bytes(src)
        else:
            with open(src, "rb") as fh:
                flat[leaf] = fh.read()

    file_lba = esp_lba + esp_blocks
    placed = []
    cursor = file_lba
    for name in sorted(flat):
        data = flat[name]
        blocks = max(1, (len(data) + BLOCK - 1) // BLOCK)
        placed.append((name, cursor, len(data), data))
        cursor += blocks
    total_blocks = cursor

    # ---- root directory ------------------------------------------------
    root = bytearray()
    root += _iso_dir_record(b"\x00", root_lba, BLOCK, True)
    root += _iso_dir_record(b"\x01", root_lba, BLOCK, True)
    for name, lba, length, _ in placed:
        iso_name = (name if "." in name else name + ".") + ";1"
        root += _iso_dir_record(iso_name, lba, length, False)
    if len(root) > BLOCK:
        raise ValueError("the ISO root directory does not fit in one block")
    root = root.ljust(BLOCK, b"\0")

    # ---- primary volume descriptor -------------------------------------
    pvd = bytearray(BLOCK)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = b" " * 32                            # system identifier
    pvd[40:72] = volume_id.ljust(32).encode("ascii")[:32]
    pvd[80:88] = _iso_both32(total_blocks)
    pvd[120:124] = _iso_both16(1)                    # volume set size
    pvd[124:128] = _iso_both16(1)                    # volume sequence number
    pvd[128:132] = _iso_both16(BLOCK)
    pvd[132:140] = _iso_both32(0)                    # path table size
    struct.pack_into("<I", pvd, 140, 0)              # L path table
    struct.pack_into(">I", pvd, 148, 0)              # M path table
    pvd[156:190] = _iso_dir_record(b"\x00", root_lba, BLOCK, True).ljust(34, b"\0")
    pvd[190:318] = b" " * 128                        # volume set identifier
    pvd[318:446] = b"KestrelOS".ljust(128)                    # publisher
    pvd[446:574] = b" " * 128
    pvd[574:702] = b" " * 128
    pvd[702:1395] = b" " * 693
    pvd[813:830] = _iso_datetime()                   # creation
    pvd[830:847] = _iso_datetime()                   # modification
    pvd[847:864] = b"0" * 16 + bytes([0])            # expiration: none
    pvd[864:881] = _iso_datetime()                   # effective
    pvd[881] = 1                                     # file structure version

    # ---- El Torito boot record ------------------------------------------
    br = bytearray(BLOCK)
    br[0] = 0
    br[1:6] = b"CD001"
    br[6] = 1
    br[7:30] = b"EL TORITO SPECIFICATION".ljust(23, b"\0")
    struct.pack_into("<I", br, 71, catalog_lba)

    # ---- terminator ------------------------------------------------------
    term = bytearray(BLOCK)
    term[0] = 0xFF
    term[1:6] = b"CD001"
    term[6] = 1

    # ---- boot catalog ----------------------------------------------------
    cat = bytearray(BLOCK)
    # Validation entry: platform 0xEF is UEFI.
    val = bytearray(32)
    val[0] = 1
    val[1] = 0xEF
    val[4:28] = b"KestrelOS".ljust(24, b"\0")
    val[30] = 0x55
    val[31] = 0xAA
    checksum = 0
    for i in range(0, 32, 2):
        checksum = (checksum + val[i] + (val[i + 1] << 8)) & 0xFFFF
    struct.pack_into("<H", val, 28, (0x10000 - checksum) & 0xFFFF)
    cat[0:32] = val

    # Initial/default entry: no emulation, the whole FAT image.
    boot = bytearray(32)
    boot[0] = 0x88                                   # bootable
    boot[1] = 0                                      # no emulation
    struct.pack_into("<H", boot, 2, 0)               # load segment: default
    boot[4] = 0                                      # system type
    # Sector count is in 512-byte units; firmware reads the whole image anyway,
    # and values over 0xFFFF are reported as the maximum by convention.
    struct.pack_into("<H", boot, 6, min(len(esp_image) // 512, 0xFFFF))
    struct.pack_into("<I", boot, 8, esp_lba)
    cat[32:64] = boot

    # ---- assemble --------------------------------------------------------
    image = bytearray(total_blocks * BLOCK)
    image[pvd_lba * BLOCK:pvd_lba * BLOCK + BLOCK] = pvd
    image[boot_record_lba * BLOCK:boot_record_lba * BLOCK + BLOCK] = br
    image[terminator_lba * BLOCK:terminator_lba * BLOCK + BLOCK] = term
    image[catalog_lba * BLOCK:catalog_lba * BLOCK + BLOCK] = cat
    image[root_lba * BLOCK:root_lba * BLOCK + BLOCK] = root
    image[esp_lba * BLOCK:esp_lba * BLOCK + len(esp_image)] = esp_image
    for _, lba, length, data in placed:
        image[lba * BLOCK:lba * BLOCK + length] = data

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(image)
    return {"blocks": total_blocks, "esp": esp.describe()}


def main():
    print(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main())

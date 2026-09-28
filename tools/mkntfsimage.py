#!/usr/bin/env python3
"""mkntfsimage.py - a bootable disk whose data partition is real NTFS.

The machine this system is carried on has an NTFS data partition, and that is
the configuration that produced "log on disk: No".  Every automated test until
now ran against a FAT data volume, which is the one arrangement that was never
the problem - so the thing most worth testing was the thing least tested.

Nothing here can format NTFS.  Windows can, and already did: build/ntfs-*.vhd
holds a volume it made.  So rather than ask for elevation to attach a virtual
disk, the formatted volume is lifted out of that image by its partition table
and dropped into a fresh GPT disk as the data partition.  The result is a
bootable image whose data volume is genuine Windows-formatted NTFS, built with
no more privilege than reading a file.

    python tools/mkntfsimage.py [source.vhd] [out.img]
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mkimage

DEFAULT_SOURCE = os.path.join(ROOT, "build", "ntfs-write.vhd")
DEFAULT_OUT = os.path.join(ROOT, "build", "vm", "kestrel-ntfs.img")


def first_partition(path):
    """The first partition in a master boot record: where it starts and how
    long it is.  The volume itself is those bytes and nothing else."""
    with open(path, "rb") as f:
        mbr = f.read(512)
        if mbr[510] != 0x55 or mbr[511] != 0xAA:
            raise SystemExit("%s has no partition table" % path)

        for i in range(4):
            e = 446 + i * 16
            if not mbr[e + 4]:
                continue
            start, count = struct.unpack_from("<II", mbr, e + 8)
            f.seek(start * 512)
            data = f.read(count * 512)
            if len(data) != count * 512:
                raise SystemExit("%s is shorter than its partition table says"
                                 % path)
            return data, mbr[e + 4]
    raise SystemExit("%s has no partitions" % path)


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SOURCE
    out = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUT

    if not os.path.exists(source):
        raise SystemExit("%s is not there - build it first" % source)

    data, kind = first_partition(source)
    if data[3:11] != b"NTFS    ":
        raise SystemExit("the first partition of %s is not NTFS" % source)

    print("taking a %.1f MiB NTFS volume (type %#x) from %s"
          % (len(data) / (1024 * 1024), kind, os.path.basename(source)))

    payload = os.path.join(ROOT, "out", "windows", "payload")
    build = os.path.join(ROOT, "build")
    esp_files = {
        "/EFI/BOOT/BOOTX64.EFI": os.path.join(build, "boot", "BOOTX64.EFI"),
        "/KESTREL/KERNEL.ELF": os.path.join(payload, "KESTREL", "KERNEL.ELF"),
        "/KESTREL/INITRD.KAR": os.path.join(payload, "KESTREL", "INITRD.KAR"),
        "/KESTREL/BOOT.CFG": os.path.join(payload, "KESTREL", "BOOT.CFG"),
        "/desktop.conf": os.path.join(ROOT, "boot", "desktop.conf"),
    }
    for name, p in esp_files.items():
        if not os.path.exists(p):
            raise SystemExit("%s is missing (%s) - run build.py first" % (name, p))

    os.makedirs(os.path.dirname(out), exist_ok=True)
    mkimage.build_disk(out, esp_files, esp_mb=64, data_raw=data)
    print("-> %s (%.0f MiB)" % (out, os.path.getsize(out) / (1024 * 1024)))

    vmdk = os.path.splitext(out)[0] + ".vmdk"
    mkimage.write_vmdk_descriptor(vmdk, out)
    print("-> %s" % vmdk)


if __name__ == "__main__":
    main()

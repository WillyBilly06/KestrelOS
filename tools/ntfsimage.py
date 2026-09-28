"""ntfsimage.py - build a disk image whose DATA partition is NTFS.

The ordinary image carries a FAT data partition, because the build has no way
to create an NTFS one - a correct NTFS volume is a great deal of machinery to
duplicate badly, and nothing here needs it.

But the stick this system is meant to live on has an NTFS data volume: the
loader and kernel on a small FAT partition because UEFI reads nothing else, and
everything that grows on NTFS beside it.  That arrangement had never been
booted.  Thousands of lines of NTFS write support had never created a file on a
mounted volume, because every machine this is tested on boots from FAT and any
NTFS disk attached to one is held read-only for not being the boot disk.

So this borrows a volume Windows already formatted - build/ntfs-test.vhd, which
the test harness makes for exactly the reason that it was created by something
other than this code - and puts it in the data partition.  Booting the result
puts the NTFS writer on the path the log takes.

    python tools/ntfsimage.py [out/kestrelos-ntfs.img]

Run `python build.py` first; this reuses what that produced rather than
rebuilding anything.
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import mkimage

SECTOR = 512


def ntfs_volume_bytes():
    """The raw NTFS VOLUME out of the harness's fixed VHD.

    Two things have to come off, and missing the second one is a trap worth
    describing because it costs a boot to notice.

    A fixed VHD is its contents followed by a 512-byte footer, so the footer
    goes first.  What is left is not a filesystem though - Windows formatted a
    whole DISK, so it begins with a master boot record and the NTFS volume
    starts further in.  Handing those bytes to a partition puts an MBR where a
    boot sector should be, and every filesystem driver correctly says it does
    not recognise it.

    So the partition table is read and only the volume inside it is taken."""
    vhd = os.path.join(ROOT, "build", "ntfs-test.vhd")
    if not os.path.exists(vhd):
        raise SystemExit(
            "build/ntfs-test.vhd is not here.  It is made by the test harness "
            "(tools/vm.ps1); run the e2e suite once, or create a 96 MiB fixed "
            "VHD formatted NTFS at that path.")

    size = os.path.getsize(vhd) - 512
    with open(vhd, "rb") as fh:
        data = fh.read(size)
    if len(data) != size:
        raise SystemExit("could not read the whole disk out of the VHD")

    if data[510:512] != b"\x55\xaa":
        raise SystemExit("that VHD has no partition table where one was expected")

    # The first entry that describes anything.  There is one; the harness makes
    # a single-partition disk.
    for i in range(4):
        e = data[446 + i * 16: 446 + (i + 1) * 16]
        kind = e[4]
        if kind == 0:
            continue
        first = int.from_bytes(e[8:12], "little")
        count = int.from_bytes(e[12:16], "little")
        if not count:
            continue

        start = first * SECTOR
        end = start + count * SECTOR
        if end > len(data):
            raise SystemExit("partition %d runs past the end of the VHD" % i)

        volume = data[start:end]
        if volume[3:11] != b"NTFS    ":
            raise SystemExit(
                "partition %d does not begin with an NTFS boot sector (%r)"
                % (i, volume[3:11]))
        return volume

    raise SystemExit("that VHD has no partitions in its table")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, "out", "kestrelos-ntfs.img")

    payload = os.path.join(ROOT, "out", "windows", "payload")
    if not os.path.isdir(payload):
        raise SystemExit("out/windows/payload is not here - run build.py first")

    # The same files the ordinary image puts on its boot partition.  Named
    # explicitly rather than walked, so a stray file in the output directory
    # cannot quietly change what boots.
    esp_files = {}
    for rel in ("EFI/BOOT/BOOTX64.EFI",
                "EFI/KESTREL/BOOTX64.EFI",
                "KESTREL/KERNEL.ELF",
                "KESTREL/INITRD.KAR",
                "KESTREL/BOOT.CFG"):
        src = os.path.join(payload, rel.replace("/", os.sep))
        if not os.path.exists(src):
            raise SystemExit("%s is missing from the payload" % rel)
        esp_files["/" + rel] = src

    conf = os.path.join(ROOT, "boot", "desktop.conf")
    if os.path.exists(conf):
        esp_files["/desktop.conf"] = conf

    volume = ntfs_volume_bytes()
    data_mb = len(volume) // (1024 * 1024)

    mkimage.build_disk(out, esp_files, esp_mb=64, data_mb=data_mb,
                       data_raw=volume)

    print("-> %s (%d MiB, data partition is NTFS, %d MiB)"
          % (os.path.relpath(out, ROOT),
             os.path.getsize(out) // (1024 * 1024), data_mb))

    vmdk = os.path.splitext(out)[0] + ".vmdk"
    mkimage.write_vmdk_descriptor(vmdk, out)
    print("-> %s" % os.path.relpath(vmdk, ROOT))


if __name__ == "__main__":
    main()

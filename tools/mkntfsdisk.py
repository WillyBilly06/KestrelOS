#!/usr/bin/env python3
"""mkntfsdisk.py - build a disk whose data volume is NTFS, using Windows to
format it.

Every test in this project runs against a data volume this build formatted
itself, which is always FAT32 - and FAT32 can create files.  The machine this
system is actually carried on has an NTFS data partition, and this driver can
change files on NTFS but cannot create them.  So the one configuration that
matters most is the one nothing exercised, and the failure it produces - a log
that cannot be opened, and everything that writes to /data quietly finding
nowhere to write - looked from the outside like logging being broken.

Windows can format NTFS and we cannot, which is the whole reason this exists:
it makes the volume with the tool that owns the format, then hands the result
to the virtual machine.  Needs elevation, because attaching a virtual disk does.

    python tools/mkntfsdisk.py build/vm/ntfsdata.vhd 512
"""
import os
import subprocess
import sys


def run_powershell(script):
    """Run a PowerShell script and return its output, or raise with the error."""
    p = subprocess.run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
        capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(p.stderr.strip() or p.stdout.strip())
    return p.stdout


def build(vhd_path, megabytes):
    vhd_path = os.path.abspath(vhd_path)
    os.makedirs(os.path.dirname(vhd_path), exist_ok=True)
    if os.path.exists(vhd_path):
        os.remove(vhd_path)

    # One partition filling the disk, formatted NTFS and labelled the way an
    # installed data volume is - the driver finds it by content rather than by
    # name, but a recognisable label makes a failure easier to read.
    script = f"""
$ErrorActionPreference = 'Stop'
$vhd = New-VHD -Path '{vhd_path}' -SizeBytes {megabytes}MB -Fixed
$d = Mount-VHD -Path '{vhd_path}' -Passthru | Get-Disk
Initialize-Disk -Number $d.Number -PartitionStyle GPT -Confirm:$false
$p = New-Partition -DiskNumber $d.Number -UseMaximumSize
Format-Volume -Partition $p -FileSystem NTFS -NewFileSystemLabel 'KESTREL' -Confirm:$false | Out-Null
Dismount-VHD -Path '{vhd_path}'
Write-Output 'ok'
"""
    run_powershell(script)
    return vhd_path


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    path = sys.argv[1]
    mb = int(sys.argv[2]) if len(sys.argv) > 2 else 512

    try:
        out = build(path, mb)
    except RuntimeError as e:
        print("could not build an NTFS disk: %s" % e)
        print("\nThis needs Hyper-V's VHD commands and elevation.  Without it "
              "the NTFS stage is skipped rather than failed: a machine that "
              "cannot make one is not a machine where this is broken.")
        return 2

    print("-> %s (%d MiB, NTFS)" % (out, mb))
    return 0


if __name__ == "__main__":
    sys.exit(main())

# installusb.ps1 - put KestrelOS onto a stick that is ALREADY partitioned.
#
# This is the non-destructive one.  provisionusb.ps1 lays a stick out from
# nothing and destroys what was on it; this assumes the layout is already right
# and only copies files onto it, so a stick with data on its system volume
# keeps it.
#
# Use this when the stick already has:
#
#     partition 1   an EFI system partition   (FAT32)
#     partition 2   a data partition          (NTFS)
#
# It will refuse if it does not find that.
#
# What it does that cannot be done without administrator, and is the whole
# reason this needs elevating:
#
#   * Writes to the EFI system partition.  Windows does not expose one as an
#     ordinary volume even when it has a drive letter, so the files cannot be
#     copied there by a normal program.
#
#   * Sets the data partition's GPT type to KestrelOS's own.  The kernel finds
#     its data volume by TYPE, not by label or letter (kernel/block.c,
#     find_data_partition), so a partition left as generic Windows data is one
#     the system will not mount - it boots, and puts its log on the boot volume
#     instead.
#
# Run it as administrator:
#
#     powershell -ExecutionPolicy Bypass -File tools\installusb.ps1 -DiskNumber 3
#
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][int]$DiskNumber,
    [string]$Payload = "$PSScriptRoot\..\out\windows\payload",
    [string]$System  = "$PSScriptRoot\..\out\windows\system"
)

$ErrorActionPreference = "Stop"

function Fail($why) {
    Write-Host ""
    Write-Host "  Stopping: $why" -ForegroundColor Red
    Write-Host ""
    exit 1
}

$GPT_ESP       = "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}"
$GPT_KESTREL   = "{b34c1a7e-9d62-4e47-9c31-5a6f2e88d140}"
$GPT_BASICDATA = "{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}"

# ---- administrator ---------------------------------------------------------
$me = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail "this needs to run as administrator - the EFI system partition cannot be written to otherwise"
}

# ---- the payload -----------------------------------------------------------
if (-not (Test-Path $Payload)) { Fail "no payload at $Payload - run 'python build.py' first" }
$Payload = (Resolve-Path $Payload).Path
foreach ($needed in @("EFI\BOOT\BOOTX64.EFI", "KESTREL\KERNEL.ELF", "KESTREL\INITRD.KAR")) {
    if (-not (Test-Path (Join-Path $Payload $needed))) {
        Fail "$needed is missing from the payload - the build did not finish"
    }
}

# ---- the disk --------------------------------------------------------------
$disk = Get-Disk -Number $DiskNumber -ErrorAction SilentlyContinue
if (-not $disk) { Fail "there is no disk $DiskNumber" }
if ($disk.IsBoot)   { Fail "disk $DiskNumber is the disk this machine boots from" }
if ($disk.IsSystem) { Fail "disk $DiskNumber holds this machine's system partition" }
if ($disk.BusType -ne "USB") {
    Fail "disk $DiskNumber is on the $($disk.BusType) bus, not USB"
}

Write-Host ""
Write-Host "  Disk  : $DiskNumber  $($disk.FriendlyName)  ($([math]::Round($disk.Size/1GB,1)) GB)"

$parts = Get-Partition -DiskNumber $DiskNumber | Sort-Object PartitionNumber
$esp  = $parts | Where-Object { $_.GptType -eq $GPT_ESP } | Select-Object -First 1
$data = $parts | Where-Object { $_.GptType -ne $GPT_ESP -and $_.Size -gt 1GB } |
        Select-Object -First 1

if (-not $esp)  { Fail "no EFI system partition on disk $DiskNumber - use provisionusb.ps1 to lay the stick out from scratch" }
if (-not $data) { Fail "no data partition on disk $DiskNumber - use provisionusb.ps1 instead" }

Write-Host "  Boot  : partition $($esp.PartitionNumber), $([math]::Round($esp.Size/1MB,0)) MB"
Write-Host "  Data  : partition $($data.PartitionNumber), $([math]::Round($data.Size/1GB,1)) GB"
Write-Host ""

# ---- reach the EFI partition ----------------------------------------------
#
# It may already have a letter and still not be reachable: Windows keeps these
# hidden.  Mounting it explicitly is what makes it an ordinary directory for
# the length of this script.
$espLetter = $esp.DriveLetter
if (-not $espLetter) {
    $used = [char[]]((Get-Volume | Where-Object DriveLetter).DriveLetter)
    foreach ($c in [char[]]"STUVWXYZ") {
        if ($used -notcontains $c) { $espLetter = $c; break }
    }
    if (-not $espLetter) { Fail "no free drive letter to mount the EFI partition on" }
    Set-Partition -DiskNumber $DiskNumber -PartitionNumber $esp.PartitionNumber `
                  -NewDriveLetter $espLetter
    Start-Sleep -Seconds 2
}

$espRoot = "${espLetter}:\"
if (-not (Test-Path $espRoot)) {
    Fail "the EFI partition is at ${espLetter}: and still cannot be reached"
}

# ---- the boot chain, without standing on the shim --------------------------
#
# This has now broken a working stick TWICE, so it is worth saying plainly.
#
# The payload contains EFI\BOOT\BOOTX64.EFI, and that file is OUR loader,
# because on a machine with Secure Boot switched off the firmware loads
# BOOTX64.EFI directly and there is nothing else to be.  But on a stick set up
# to pass Secure Boot, BOOTX64.EFI is Microsoft's signed `shim`, and the chain
# runs shim -> grubx64.efi -> kernel, with our loader sitting at grubx64.efi
# and its HASH enrolled through MokManager.
#
# A recursive copy of the payload over that stick replaces the one signed file
# in the chain with an unsigned one.  The firmware then refuses to start
# anything at all:
#
#     Secure Boot Violation
#     Invalid signature detected. Check Secure Boot Policy in Setup
#
# and it looks like the kernel is at fault when nothing of the kernel ran.
#
# So the shim is detected and stepped around.  It is recognised by not being
# our own loader - a byte comparison against the payload's copy, rather than a
# size or a name, because any test that can be fooled will eventually be.
#
# Both boot paths are handled.  The payload ships EFI\BOOT\BOOTX64.EFI, which
# is what firmware runs when a removable disk is chosen from the boot menu, and
# EFI\KESTREL\BOOTX64.EFI, which is where a named firmware boot entry points.
# Leaving the second one unsigned is a trap: the stick boots from the menu and
# fails from the entry, which reads as an intermittent fault rather than a
# missing signature.
$bootPaths = @("EFI\BOOT", "EFI\KESTREL")

$ourBoot = Join-Path $Payload "EFI\BOOT\BOOTX64.EFI"
# Whenever a signed shim is available, ALWAYS lay the Secure Boot chain (shim at
# BOOTX64.EFI, our loader at grubx64.efi).  The previous logic only restored the
# shim when the stick's BOOTX64.EFI DIFFERED from our loader - so the moment a
# recovery left our unsigned loader sitting at BOOTX64.EFI, every later flash saw
# "same as ours", skipped the shim, and perpetuated an Invalid-Signature stick.
# A shimmed stick boots correctly with Secure Boot either on or off, so placing
# the shim unconditionally is safe.
$shimmed = Test-Path "$PSScriptRoot\..\secureboot\shimx64.signed.efi"

Write-Host "  Copying the boot chain onto ${espLetter}: ..."
Copy-Item -Path (Join-Path $Payload "*") -Destination $espRoot -Recurse -Force

if ($shimmed) {
    # Put the signed loader back and route our own to where shim looks for it.
    $signed = "$PSScriptRoot\..\secureboot\shimx64.signed.efi"
    if (-not (Test-Path $signed)) {
        Fail "this stick boots through a signed shim and secureboot\shimx64.signed.efi is missing, so the shim just overwritten cannot be put back.  Restore EFI\BOOT\BOOTX64.EFI from a copy before booting."
    }
    $mok = "$PSScriptRoot\..\secureboot\mmx64.efi"
    foreach ($rel in $bootPaths) {
        $dir = Join-Path $espRoot $rel
        if (-not (Test-Path $dir)) { continue }
        Copy-Item $signed  (Join-Path $dir "BOOTX64.EFI") -Force
        Copy-Item $ourBoot (Join-Path $dir "grubx64.efi") -Force
        Copy-Item $ourBoot (Join-Path $dir "kestrel.efi") -Force
        # MokManager, so the hash can be re-enrolled without another machine.
        if ((Test-Path $mok) -and -not (Test-Path (Join-Path $dir "mmx64.efi"))) {
            Copy-Item $mok (Join-Path $dir "mmx64.efi") -Force
        }
        Write-Host ("    Secure Boot: {0} keeps the signed shim at BOOTX64.EFI," -f $rel) -ForegroundColor Cyan
        Write-Host  "    with this system's loader at grubx64.efi where shim looks." -ForegroundColor Cyan
    }
}

Write-Host "    done" -ForegroundColor Green

# ---- the data partition ----------------------------------------------------
#
# On a re-run this partition is already typed as this system's data volume, and
# Windows then refuses to give it a drive letter - so it cannot be written to as
# an ordinary volume, and the system files and firmware below get silently
# skipped.  That is exactly how a stick ends up booting with no GSP firmware on
# it.  So if it has no letter, flip it to a plain basic-data partition just long
# enough to mount it (the contents are untouched by a type change); the
# type-marking step further down sets it back to ours.
$dataLetter = $data.DriveLetter
if (-not $dataLetter) {
    Write-Host "  Data partition has no drive letter (already typed as ours) - mounting it..."
    Set-Partition -DiskNumber $DiskNumber -PartitionNumber $data.PartitionNumber `
                  -GptType $GPT_BASICDATA
    Start-Sleep -Seconds 2
    $data = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $data.PartitionNumber
    $dataLetter = $data.DriveLetter
    if (-not $dataLetter) {
        $used = [char[]]((Get-Volume | Where-Object DriveLetter).DriveLetter)
        foreach ($c in [char[]]"STUVWXYZ") {
            if ($used -notcontains $c) { $dataLetter = $c; break }
        }
        if ($dataLetter) {
            Set-Partition -DiskNumber $DiskNumber -PartitionNumber $data.PartitionNumber `
                          -NewDriveLetter $dataLetter
            Start-Sleep -Seconds 2
        }
    }
}
if (-not ($dataLetter -and (Test-Path "${dataLetter}:\"))) {
    Fail "the data partition could not be mounted, so the system files and firmware could not be copied - the stick would boot without its GSP firmware.  Use provisionusb.ps1 to lay the stick out again."
}
if ($dataLetter -and (Test-Path "${dataLetter}:\")) {
    if (Test-Path $System) {
        Write-Host "  Copying the system files onto ${dataLetter}: ..."
        New-Item -ItemType Directory -Path "${dataLetter}:\system" -Force | Out-Null
        Copy-Item -Path (Join-Path $System "*") -Destination "${dataLetter}:\system\" `
                  -Recurse -Force
    }
    # The kernel creates files but not the directory above them.
    New-Item -ItemType Directory -Path "${dataLetter}:\logs" -Force | Out-Null

    # Large vendor firmware that is too big for the initial archive - the 60 MB
    # NVIDIA GSP-RM image - travels on the data volume instead, at the path the
    # kernel reads it from (/data/firmware maps to the root of this volume).
    # Kept out of the build tree so it does not bloat every image; supplied
    # under the repo's staging\firmware and copied here.
    $fwStage = Join-Path $PSScriptRoot "..\staging\firmware"
    $fwStage = [System.IO.Path]::GetFullPath($fwStage)
    if (Test-Path $fwStage) {
        $fwDest = "${dataLetter}:\firmware"
        Write-Host "  Copying vendor firmware onto $fwDest ..."
        New-Item -ItemType Directory -Path $fwDest -Force | Out-Null
        Copy-Item -Path (Join-Path $fwStage "*") -Destination "$fwDest\" -Recurse -Force
        Write-Host "    firmware copied" -ForegroundColor Green
        # Prove the big GSP-RM blobs actually landed - a silent partial copy here
        # is what leaves the co-processor with nothing to run.
        $gspDir = Join-Path $fwDest "nvidia\gb202\gsp"
        Get-ChildItem $gspDir -Filter *.bin -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Host ("      {0}  {1:N1} MB" -f $_.Name, ($_.Length/1MB)) -ForegroundColor Green
        }
        if (-not (Get-ChildItem $gspDir -Filter *.bin -ErrorAction SilentlyContinue)) {
            Fail "the GSP firmware did not copy onto ${dataLetter}:\firmware - the stick would boot without it"
        }
    }
    Write-Host "    done" -ForegroundColor Green
}

# ---- the type the kernel looks for -----------------------------------------
if ($data.GptType -ne $GPT_KESTREL) {
    Write-Host "  Marking partition $($data.PartitionNumber) as this system's data volume..."
    # Changing the type is not changing the contents: the filesystem, its label
    # and everything on it are untouched.  Windows may stop showing it as an
    # ordinary drive afterwards, which is expected - it is no longer claiming
    # to be a Windows data partition.
    Set-Partition -DiskNumber $DiskNumber -PartitionNumber $data.PartitionNumber `
                  -GptType $GPT_KESTREL
    Write-Host "    done - Windows may no longer show it as a drive, which is correct" -ForegroundColor Green
} else {
    Write-Host "  Partition $($data.PartitionNumber) is already marked as this system's data volume."
}

Write-Host ""
Write-Host "  Ready.  Restart and choose the USB stick in the firmware's boot menu." -ForegroundColor Green
Write-Host ""

# provisionusb.ps1 - lay a USB stick out the way this system expects to live on
# it: a FAT boot volume and an NTFS volume for everything else.
#
#     KESTREL      (FAT32)  the EFI system partition.  The firmware reads the
#                           loader, the kernel and the initial archive from
#                           here, and it MUST be FAT - UEFI firmware reads no
#                           other filesystem, which is why Windows and Linux
#                           both keep a small FAT partition too.
#
#     KESTRELDATA  (NTFS)   everything the kernel reads after it is running:
#                           the log, settings, saved state, user files.  This
#                           is the volume that grows; the boot one does not.
#
# Why this exists beside writeusb.ps1.  That script writes a disk image over
# the whole stick, and the image carries a FAT data partition because the build
# has no way to make an NTFS one - creating a valid NTFS volume from nothing is
# a great deal of work to duplicate badly.  Windows already has that code.  So
# this asks Windows to make the volumes and then fills them, which produces a
# stick this system can both boot from AND write to properly.
#
# Run it as administrator:
#
#     powershell -ExecutionPolicy Bypass -File tools\provisionusb.ps1 -DiskNumber 3
#
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][int]$DiskNumber,
    [string]$Payload  = "$PSScriptRoot\..\out\windows\payload",
    [int]$BootMB      = 512,
    [string]$BootLetter = "G",
    [string]$DataLetter = "K",
    [switch]$Yes
)

$ErrorActionPreference = "Stop"

function Fail($why) {
    Write-Host ""
    Write-Host "  Refusing to continue: $why" -ForegroundColor Red
    Write-Host ""
    exit 1
}

# The partition types this system looks for.  The data one is KestrelOS's own:
# kernel/block.c GPT_TYPE_KESTREL, and find_data_partition() searches for it, so
# a stick whose data partition carries any other type is a stick the kernel
# mounts nothing from.
$GPT_ESP     = "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}"
$GPT_KESTREL = "{b34c1a7e-9d62-4e47-9c31-5a6f2e88d140}"

# ---- administrator ---------------------------------------------------------
$me = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail "this needs to run as administrator - partitioning a disk is not something an ordinary program is allowed to do"
}

# ---- the payload -----------------------------------------------------------
if (-not (Test-Path $Payload)) {
    Fail "there is nothing to install at $Payload - run 'python build.py' first"
}
$Payload = (Resolve-Path $Payload).Path
foreach ($needed in @("EFI\BOOT\BOOTX64.EFI", "KESTREL\KERNEL.ELF", "KESTREL\INITRD.KAR")) {
    if (-not (Test-Path (Join-Path $Payload $needed))) {
        Fail "$needed is missing from the payload - the build did not finish"
    }
}

# ---- the disk, checked rather than trusted ---------------------------------
#
# Every one of these has cost somebody a disk somewhere.  A disk number is not
# stable across replugs: the number that was a memory stick a minute ago can be
# the disk with everything on it now.
$disk = Get-Disk -Number $DiskNumber -ErrorAction SilentlyContinue
if (-not $disk) { Fail "there is no disk $DiskNumber" }

Write-Host ""
Write-Host "  Disk  : $DiskNumber  $($disk.FriendlyName)"
Write-Host "          $([math]::Round($disk.Size/1GB,2)) GB, $($disk.BusType), $($disk.PartitionStyle)"

if ($disk.IsBoot)   { Fail "disk $DiskNumber is the disk this machine boots from" }
if ($disk.IsSystem) { Fail "disk $DiskNumber holds this machine's system partition" }
if ($disk.BusType -ne "USB") {
    Fail "disk $DiskNumber is on the $($disk.BusType) bus, not USB - this only writes to removable sticks"
}
if ($disk.Size -gt 512GB) {
    Fail "disk $DiskNumber is $([math]::Round($disk.Size/1GB,0)) GB, which is larger than any stick this expects"
}
if ($disk.Size -lt 2GB) {
    Fail "disk $DiskNumber is only $([math]::Round($disk.Size/1GB,2)) GB; the two volumes need more room than that"
}

# ---- what is on it now, said out loud --------------------------------------
Write-Host ""
Write-Host "  Everything on this disk will be destroyed:" -ForegroundColor Yellow
$existing = Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue
if ($existing) {
    foreach ($p in $existing) {
        $v = Get-Volume -Partition $p -ErrorAction SilentlyContinue
        $label = if ($v -and $v.FileSystemLabel) { $v.FileSystemLabel } else { "(no label)" }
        $fs = if ($v) { $v.FileSystem } else { "unformatted" }
        Write-Host ("    partition {0}  {1,8}  {2,-8}  {3}" -f `
            $p.PartitionNumber, "$([math]::Round($p.Size/1GB,2)) GB", $fs, $label)
    }
} else {
    Write-Host "    (no partitions)"
}
Write-Host ""

if (-not $Yes) {
    $answer = Read-Host "  Type the disk number again to confirm"
    if ($answer -ne "$DiskNumber") { Fail "that did not match" }
}

# ---- lay it out ------------------------------------------------------------
Write-Host ""
Write-Host "  Clearing disk $DiskNumber..."
Clear-Disk -Number $DiskNumber -RemoveData -RemoveOEM -Confirm:$false
Initialize-Disk -Number $DiskNumber -PartitionStyle GPT -Confirm:$false | Out-Null

Write-Host "  Creating KESTREL ($BootMB MB, FAT32, EFI system partition)..."
$boot = New-Partition -DiskNumber $DiskNumber -Size ($BootMB * 1MB) -GptType $GPT_ESP
Format-Volume -Partition $boot -FileSystem FAT32 -NewFileSystemLabel "KESTREL" `
              -Confirm:$false | Out-Null

Write-Host "  Creating KESTRELDATA (the rest of the stick, NTFS)..."
$data = New-Partition -DiskNumber $DiskNumber -UseMaximumSize -GptType $GPT_KESTREL
Format-Volume -Partition $data -FileSystem NTFS -NewFileSystemLabel "KESTRELDATA" `
              -Confirm:$false | Out-Null

# Drive letters, preferred but not insisted on.  A letter already in use is not
# a reason to stop - the volumes are found by their partition type and label,
# not by which letter Windows happened to give them.
function Assign($partition, $wanted, $what) {
    $inUse = (Get-Volume -ErrorAction SilentlyContinue |
              Where-Object { $_.DriveLetter } | ForEach-Object { $_.DriveLetter })
    if ($inUse -contains $wanted[0]) {
        Write-Host "  $($wanted): is already in use, so $what keeps whatever letter Windows gives it" -ForegroundColor Yellow
        return (Get-Partition -DiskNumber $partition.DiskNumber -PartitionNumber $partition.PartitionNumber).DriveLetter
    }
    Set-Partition -DiskNumber $partition.DiskNumber `
                  -PartitionNumber $partition.PartitionNumber `
                  -NewDriveLetter $wanted
    return $wanted
}

$bootDrive = Assign $boot $BootLetter "the boot volume"
$dataDrive = Assign $data $DataLetter "the system volume"

Start-Sleep -Seconds 2

# ---- fill them -------------------------------------------------------------
Write-Host ""
Write-Host "  Copying the boot chain onto ${bootDrive}: ..."
Copy-Item -Path (Join-Path $Payload "*") -Destination "${bootDrive}:\" -Recurse -Force

# The desktop's configuration travels with the boot volume, because it is read
# before the data volume is mounted.
$conf = Join-Path (Split-Path $Payload -Parent) "payload\desktop.conf"
if (Test-Path $conf) { Copy-Item $conf "${bootDrive}:\KESTREL\" -Force }

Write-Host "  Preparing ${dataDrive}: ..."
# The log directory has to exist before the kernel writes into it; it creates
# files but not the directory above them.
New-Item -ItemType Directory -Path "${dataDrive}:\logs" -Force | Out-Null
New-Item -ItemType Directory -Path "${dataDrive}:\system" -Force | Out-Null
New-Item -ItemType Directory -Path "${dataDrive}:\home" -Force | Out-Null

# Whatever the build staged for the system volume - the icon font, and
# anything that joins it later.  These are files the kernel reads once the
# disks are up, so they belong here rather than on the boot partition, and
# keeping them on a volume rather than inside the binary is what lets somebody
# replace them.
$system = Join-Path (Split-Path $Payload -Parent) "system"
if (Test-Path $system) {
    Write-Host "  Copying the system files onto ${dataDrive}: ..."
    Copy-Item -Path (Join-Path $system "*") -Destination "${dataDrive}:\system\" `
              -Recurse -Force
}

# Something to prove the volume was written by this script and not left over
# from whatever the stick held before.
$stamp = "KestrelOS system volume, prepared {0}" -f (Get-Date -Format "yyyy-MM-dd HH:mm")
Set-Content -Path "${dataDrive}:\system\VOLUME.TXT" -Value $stamp -Encoding utf8

Write-Host ""
Write-Host "  Done." -ForegroundColor Green
Write-Host ""
Write-Host "    ${bootDrive}:  KESTREL      FAT32  the loader, the kernel, the initial archive"
Write-Host "    ${dataDrive}:  KESTRELDATA  NTFS   the log, settings, system files and user files"
Write-Host ""
Write-Host "  The kernel finds the second one by its partition type, not its letter,"
Write-Host "  so it will mount at /data whatever Windows calls it here."
Write-Host ""

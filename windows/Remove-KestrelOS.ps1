<#
    Remove-KestrelOS.ps1 - undo what Install-KestrelOS.ps1 staged.

    Removes the firmware boot entry and deletes the KestrelOS files from the
    EFI system partition.  It does not touch partitions: space that was freed
    by shrinking stays unallocated, and any partitions the KestrelOS installer
    created are left alone.  Both are shown at the end so they can be dealt
    with in Disk Management, deliberately rather than by surprise.
#>
[CmdletBinding()]
param([switch]$Force)

$ErrorActionPreference = "Stop"
$script:EspLetter = $null

function Say([string]$t, [string]$c = "Gray") { Write-Host $t -ForegroundColor $c }
function Title([string]$t) {
    Write-Host ""
    Write-Host "  $t" -ForegroundColor Cyan
    Write-Host "  $('-' * $t.Length)" -ForegroundColor DarkGray
}

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Say "  This needs to run as Administrator." Red
    exit 1
}

Clear-Host
Write-Host ""
Write-Host "  Remove KestrelOS from the Windows boot configuration" -ForegroundColor Cyan
Write-Host "  ===================================================" -ForegroundColor DarkGray
Write-Host ""

if (-not $Force) {
    $go = Read-Host "  Remove the KestrelOS boot entry and loader files? (y/N)"
    if ($go -ne "y" -and $go -ne "Y") { Say "  Cancelled."; exit 0 }
}

# ---- boot entry ----
Title "Firmware boot entry"
$recorded = Join-Path $PSScriptRoot "kestrel-boot-entry.txt"
$guid = $null
if (Test-Path $recorded) { $guid = (Get-Content $recorded -Raw).Trim() }

if (-not $guid) {
    # Fall back to finding it by description.
    $listing = & bcdedit /enum firmware 2>&1 | Out-String
    $blocks = $listing -split "(?m)^\s*$"
    foreach ($b in $blocks) {
        if ($b -match "KestrelOS" -and $b -match "identifier\s+(\{[0-9a-fA-F-]{36}\})") {
            $guid = $matches[1]
            break
        }
    }
}

if ($guid) {
    & bcdedit /delete $guid /f 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Say "  Removed boot entry $guid" Green
        if (Test-Path $recorded) { Remove-Item $recorded -Force }
    } else {
        Say "  bcdedit could not remove $guid; it may already be gone." Yellow
    }
} else {
    Say "  No KestrelOS boot entry was found." DarkGray
}

# ---- files on the ESP ----
Title "Loader files"
$used = (Get-Volume | Where-Object DriveLetter).DriveLetter
$letter = $null
foreach ($c in [char[]]"STUVWXYZ") { if ($used -notcontains $c) { $letter = $c; break } }

if ($letter) {
    & mountvol "$letter`:" /S 2>&1 | Out-Null
    if (Test-Path "$letter`:\") {
        $script:EspLetter = $letter
        $removed = 0
        foreach ($p in @("$letter`:\EFI\KESTREL", "$letter`:\KESTREL")) {
            if (Test-Path $p) { Remove-Item $p -Recurse -Force; $removed++; Say "  Removed $p" }
        }
        # \EFI\BOOT\BOOTX64.EFI is shared with other systems and is deliberately
        # left alone: deleting it can make the machine unbootable.
        if ($removed -eq 0) { Say "  Nothing to remove." DarkGray }
        & mountvol "$letter`:" /D 2>&1 | Out-Null
    } else {
        Say "  Could not mount the EFI system partition." Yellow
    }
} else {
    Say "  No spare drive letter to mount the EFI partition with." Yellow
}

# ---- what is left ----
Title "What is left on your disks"
$kestrel = Get-Partition | Where-Object { $_.GptType -eq "{b34c1a7e-9d62-4e47-9c31-5a6f2e88d140}" }
if ($kestrel) {
    Say "  These KestrelOS partitions still exist:" Yellow
    $kestrel | ForEach-Object {
        Say ("    disk {0} partition {1}, {2:N1} GB" -f $_.DiskNumber, $_.PartitionNumber, ($_.Size / 1GB))
    }
    Say "  Delete them in Disk Management if you want the space back." DarkGray
} else {
    Say "  No KestrelOS partitions were found." DarkGray
}

Write-Host ""
Say "  Windows boots normally again." Green
Write-Host ""

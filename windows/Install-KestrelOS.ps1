<#
    Install-KestrelOS.ps1 - set KestrelOS up from inside Windows.

    What it does, in order:

      1. checks that this machine booted UEFI (KestrelOS has no BIOS loader)
      2. shows the disks and how much each can give up
      3. asks how much space to set aside, bounded by what Windows says is
         actually shrinkable
      4. shrinks the chosen volume, leaving the freed space unallocated
      5. copies the loader and the system image onto the EFI system partition
      6. adds a firmware boot entry and marks it for the next boot only
      7. offers to restart

    Nothing is shrunk or copied until a summary has been shown and confirmed.
    The shrink is the only destructive-sounding step, and Windows performs it
    itself: it moves no files it cannot move and refuses rather than risk data.

    After the restart, KestrelOS starts and its own installer creates its
    partitions in the space freed here.

    Run it from an elevated PowerShell, or just double-click Install-KestrelOS.cmd.
#>
[CmdletBinding()]
param(
    [int]$SizeGB = 0,           # 0 means "ask"
    [switch]$NoReboot,
    [switch]$Force              # skip the confirmation prompt
)

$ErrorActionPreference = "Stop"
$script:EspLetter = $null
$script:MountedEsp = $false

# --------------------------------------------------------------------- output

function Say([string]$text, [string]$colour = "Gray") { Write-Host $text -ForegroundColor $colour }
function Title([string]$text) {
    Write-Host ""
    Write-Host "  $text" -ForegroundColor Cyan
    Write-Host "  $('-' * $text.Length)" -ForegroundColor DarkGray
}
function Warn([string]$text) { Write-Host "  $text" -ForegroundColor Yellow }
function Fail([string]$text) {
    Write-Host ""
    Write-Host "  $text" -ForegroundColor Red
    Write-Host ""
    Cleanup
    exit 1
}
function Bytes([double]$n) {
    $units = "B", "KB", "MB", "GB", "TB"
    $i = 0
    while ($n -ge 1024 -and $i -lt 4) { $n /= 1024; $i++ }
    if ($n -lt 10 -and $i -gt 0) { return "{0:N1} {1}" -f $n, $units[$i] }
    return "{0:N0} {1}" -f $n, $units[$i]
}

# ------------------------------------------------------------------- checks

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Fail "This needs to run as Administrator.  Right-click Install-KestrelOS.cmd and choose 'Run as administrator'."
    }
}

function Assert-Uefi {
    # An MBR system disk, or a missing EFI variable store, means a BIOS boot.
    try {
        $firmware = $env:firmware_type
        if (-not $firmware) {
            $firmware = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control" -Name "PEFirmwareType" -ErrorAction SilentlyContinue).PEFirmwareType
            $firmware = if ($firmware -eq 2) { "UEFI" } elseif ($firmware -eq 1) { "Legacy" } else { $null }
        }
    } catch { $firmware = $null }

    if ($firmware -and $firmware -ne "UEFI") {
        Fail "This computer started in legacy BIOS mode.  KestrelOS boots through UEFI only."
    }
    if (-not $firmware) {
        Warn "Could not confirm the firmware type; continuing on the assumption that this is UEFI."
    }
}

function Get-SecureBootState {
    try { return Confirm-SecureBootUEFI } catch { return $null }
}

# ---------------------------------------------------------------------- ESP

function Mount-Esp {
    # The EFI system partition has no drive letter by default.  mountvol can
    # give it one; a letter that is already free is chosen so nothing is
    # displaced.
    $used = (Get-Volume | Where-Object DriveLetter).DriveLetter
    $letter = $null
    foreach ($c in [char[]]"STUVWXYZ") { if ($used -notcontains $c) { $letter = $c; break } }
    if (-not $letter) { Fail "No spare drive letter is available to mount the EFI partition." }

    & mountvol "$letter`:" /S 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$letter`:\")) {
        Fail "Could not mount the EFI system partition (mountvol returned $LASTEXITCODE)."
    }
    $script:EspLetter = $letter
    $script:MountedEsp = $true
    Say "  EFI system partition mounted as $letter`:"
    return "$letter`:"
}

function Cleanup {
    if ($script:MountedEsp -and $script:EspLetter) {
        & mountvol "$($script:EspLetter):" /D 2>&1 | Out-Null
        $script:MountedEsp = $false
    }
}

# ------------------------------------------------------------------- volumes

function Get-Candidates {
    $out = @()
    foreach ($p in Get-Partition | Where-Object { $_.DriveLetter }) {
        $vol = Get-Volume -Partition $p -ErrorAction SilentlyContinue
        if (-not $vol -or $vol.FileSystemType -ne "NTFS") { continue }

        $min = $null
        $max = $null
        try {
            $supported = Get-PartitionSupportedSize -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber -ErrorAction Stop
            $min = $supported.SizeMin
            $max = $supported.SizeMax
        } catch { continue }

        # Windows reports the smallest size the volume can be shrunk to; the
        # difference is what can actually be freed, whatever the free space says.
        $shrinkable = $p.Size - $min
        if ($shrinkable -lt 1GB) { continue }

        $out += [pscustomobject]@{
            Disk        = $p.DiskNumber
            Partition   = $p.PartitionNumber
            Letter      = $p.DriveLetter
            Label       = $vol.FileSystemLabel
            Size        = $p.Size
            Free        = $vol.SizeRemaining
            Shrinkable  = $shrinkable
            IsSystem    = ($p.DriveLetter -eq ($env:SystemDrive -replace ':', ''))
        }
    }
    return $out
}

# ---------------------------------------------------------------------- main

Assert-Admin
Assert-Uefi

Clear-Host
Write-Host ""
Write-Host "  KestrelOS installer for Windows" -ForegroundColor Cyan
Write-Host "  ===============================" -ForegroundColor DarkGray
Write-Host ""
Say "  This sets aside space on a disk and stages the KestrelOS loader so that"
Say "  the computer starts into it on the next restart.  Windows is left"
Say "  installed and bootable."
Write-Host ""

$payload = Join-Path $PSScriptRoot "payload"
if (-not (Test-Path (Join-Path $payload "EFI\BOOT\BOOTX64.EFI"))) {
    Fail "The payload folder is missing or incomplete.  Expected $payload\EFI\BOOT\BOOTX64.EFI"
}

$secureBoot = Get-SecureBootState
if ($secureBoot -eq $true) {
    Title "Secure Boot"
    Warn "Secure Boot is switched on."
    Say  "  The KestrelOS loader is not signed by a certificate this computer trusts,"
    Say  "  so firmware will refuse to start it.  Turn Secure Boot off in firmware"
    Say  "  setup before restarting, or this installation will not boot."
    Write-Host ""
    if (-not $Force) {
        $go = Read-Host "  Continue anyway? (y/N)"
        if ($go -ne "y" -and $go -ne "Y") { Cleanup; exit 0 }
    }
}

# ---- choose a volume to shrink ----
Title "Choose where to take the space from"

$candidates = Get-Candidates
if (-not $candidates) {
    Fail "No NTFS volume on this computer can be shrunk by at least 1 GB.  Free up space in Windows and try again."
}

$i = 1
$table = foreach ($c in $candidates) {
    [pscustomobject]@{
        "#"          = $i++
        Volume       = "$($c.Letter): $($c.Label)"
        Size         = Bytes $c.Size
        Free         = Bytes $c.Free
        "Can free"   = Bytes $c.Shrinkable
        Windows      = if ($c.IsSystem) { "yes" } else { "" }
    }
}
$table | Format-Table -AutoSize | Out-String | Write-Host

$choice = 1
if ($candidates.Count -gt 1) {
    do {
        $answer = Read-Host "  Which volume? (1-$($candidates.Count))"
        $choice = 0
        [void][int]::TryParse($answer, [ref]$choice)
    } while ($choice -lt 1 -or $choice -gt $candidates.Count)
}
$target = $candidates[$choice - 1]

# ---- how much ----
Title "How much space should KestrelOS have?"

$maxGB = [math]::Floor($target.Shrinkable / 1GB)
$minGB = 8
if ($maxGB -lt $minGB) { Fail "Only $(Bytes $target.Shrinkable) can be freed from $($target.Letter): and KestrelOS needs at least $minGB GB." }

Say "  $($target.Letter): can give up at most $(Bytes $target.Shrinkable)."
Say "  KestrelOS needs at least $minGB GB; 32 GB is comfortable."
Write-Host ""

if ($SizeGB -le 0) {
    do {
        $answer = Read-Host "  Size in GB ($minGB-$maxGB)"
        $SizeGB = 0
        [void][int]::TryParse($answer, [ref]$SizeGB)
        if ($SizeGB -lt $minGB -or $SizeGB -gt $maxGB) {
            Warn "  Enter a whole number between $minGB and $maxGB."
            $SizeGB = 0
        }
    } while ($SizeGB -le 0)
} elseif ($SizeGB -lt $minGB -or $SizeGB -gt $maxGB) {
    Fail "The requested $SizeGB GB is outside what can be freed ($minGB-$maxGB GB)."
}

$shrinkBytes = [int64]$SizeGB * 1GB
$newSize = $target.Size - $shrinkBytes

# ---- confirm ----
Title "Summary"
Say "  Volume to shrink   : $($target.Letter): $($target.Label)"
Say "  Current size       : $(Bytes $target.Size)"
Say "  New size           : $(Bytes $newSize)"
Say "  Freed for KestrelOS: $(Bytes $shrinkBytes)"
Write-Host ""
Say "  The freed space is left unallocated.  Windows keeps every file it has;"
Say "  the KestrelOS installer will create its partitions there after the restart."
Write-Host ""

if (-not $Force) {
    $confirm = Read-Host "  Type YES to go ahead"
    if ($confirm -cne "YES") { Say "  Cancelled.  Nothing has been changed."; Cleanup; exit 0 }
}

# ---- shrink ----
Title "Shrinking $($target.Letter):"
try {
    Resize-Partition -DiskNumber $target.Disk -PartitionNumber $target.Partition -Size $newSize -ErrorAction Stop
    Say "  Done.  $(Bytes $shrinkBytes) is now unallocated on disk $($target.Disk)." "Green"
} catch {
    Fail ("Windows could not shrink the volume: " + $_.Exception.Message + "`n" +
          "  This usually means unmovable files sit near the end of the volume.`n" +
          "  Disabling hibernation and the page file, restarting, and trying again`n" +
          "  normally clears it.")
}

# ---- stage the loader ----
Title "Copying the loader to the EFI system partition"
$esp = Mount-Esp

try {
    foreach ($dir in @("$esp\EFI\KESTREL", "$esp\KESTREL")) {
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    }

    $files = @(
        @{ From = "EFI\BOOT\BOOTX64.EFI";    To = "$esp\EFI\KESTREL\BOOTX64.EFI"; What = "loader" },
        @{ From = "KESTREL\KERNEL.ELF";      To = "$esp\KESTREL\KERNEL.ELF";      What = "kernel" },
        @{ From = "KESTREL\INITRD.KAR";      To = "$esp\KESTREL\INITRD.KAR";      What = "system image" },
        @{ From = "KESTREL\BOOT.CFG";        To = "$esp\KESTREL\BOOT.CFG";        What = "boot configuration" }
    )
    foreach ($f in $files) {
        $src = Join-Path $payload $f.From
        if (-not (Test-Path $src)) { Fail "Missing from the payload: $($f.From)" }
        Copy-Item $src $f.To -Force
        Say ("  {0,-22} {1}" -f $f.What, (Bytes (Get-Item $f.To).Length))
    }

    # ---- firmware boot entry ----
    Title "Adding a firmware boot entry"

    $espPartition = Get-Partition | Where-Object { $_.DiskNumber -eq $target.Disk -and $_.GptType -eq "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}" } | Select-Object -First 1
    if (-not $espPartition) {
        Warn "  No EFI system partition was found on disk $($target.Disk)."
        Warn "  The files were copied to the EFI partition Windows uses instead."
    }

    $out = & bcdedit /copy "{bootmgr}" /d "KestrelOS" 2>&1
    $guid = $null
    if ($out -match '(\{[0-9a-fA-F-]{36}\})') { $guid = $matches[1] }

    if ($guid) {
        & bcdedit /set $guid path "\EFI\KESTREL\BOOTX64.EFI" 2>&1 | Out-Null
        & bcdedit /set "{fwbootmgr}" bootsequence $guid 2>&1 | Out-Null
        Say "  Added boot entry $guid and set it for the next restart only." "Green"
        Say "  Windows remains the default; this affects one boot." "DarkGray"
        Set-Content -Path (Join-Path $PSScriptRoot "kestrel-boot-entry.txt") -Value $guid -Encoding ascii
    } else {
        Warn "  Could not create a firmware boot entry:"
        Warn "    $out"
        Say  "  You can still start KestrelOS from the firmware boot menu"
        Say  "  (usually F12, F9 or Esc during start-up)."
    }
} finally {
    Cleanup
}

# ---- done ----
Title "Ready"
Say "  KestrelOS is staged and $(Bytes $shrinkBytes) is waiting for it on disk $($target.Disk)." "Green"
Write-Host ""
Say "  On the next restart the computer starts KestrelOS.  Choose"
Say "  'Install KestrelOS' from its menu and pick 'Use unallocated space'"
Say "  when it asks - it will find the space freed here."
Write-Host ""
Say "  To undo everything staged here, run Remove-KestrelOS.ps1." "DarkGray"
Write-Host ""

if ($NoReboot) { Say "  Restart when you are ready."; exit 0 }

$answer = Read-Host "  Restart now? (y/N)"
if ($answer -eq "y" -or $answer -eq "Y") {
    Say "  Restarting in 5 seconds.  Close anything you have open." "Yellow"
    Start-Sleep -Seconds 5
    Restart-Computer -Force
} else {
    Say "  Restart when you are ready."
}

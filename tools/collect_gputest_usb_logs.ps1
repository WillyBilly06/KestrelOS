param([int]$DiskNumber = 3, [switch]$HideAfter)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$disk = Get-Disk -Number $DiskNumber
if ($disk.FriendlyName.Trim() -ne 'USB DISK 3.0' -or
    $disk.SerialNumber.Trim() -ne '087F20115040' -or
    $disk.Size -ne 123942731776 -or $disk.BusType -ne 'USB' -or
    $disk.IsBoot -or $disk.IsSystem) {
    throw 'refusing to mount: Disk 3 identity/safety tuple does not match'
}
$used = @(Get-Volume | Where-Object DriveLetter | ForEach-Object { [string]$_.DriveLetter })
$partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber 1
$letter = if ([int][char]$partition.DriveLetter -gt 0) { [string]$partition.DriveLetter } else { $null }
if (-not $letter) {
    foreach ($candidate in @('S','Z','Y','X','W','V')) {
        if ($used -notcontains $candidate) { $letter = $candidate; break }
    }
}
if (-not $letter) { throw 'no temporary drive letter available' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$destination = Join-Path $root "out\stick\_logs\$stamp"
New-Item -ItemType Directory -Force -Path $destination | Out-Null
try {
    # A visible ESP already owns this letter; assigning it again can fail with
    # "requested access path is already in use" even when it is our partition.
    if ([int][char]$partition.DriveLetter -eq 0) {
        Set-Partition -DiskNumber $DiskNumber -PartitionNumber 1 -NewDriveLetter $letter
    }
    Start-Sleep -Seconds 1
    $source = "${letter}:\KESTREL"
    foreach ($name in @('KERNEL.LOG','gpu-boot.log','tri.raw')) {
        $path = Join-Path $source $name
        if (Test-Path -LiteralPath $path) {
            Copy-Item -LiteralPath $path -Destination $destination
        }
    }
    $files = @(Get-ChildItem -LiteralPath $destination -File)
    if (-not $files.Count) { throw 'no Kestrel hardware logs were found on the ESP' }
    "USB_LOG_DIR=$destination"
    $files | ForEach-Object { "COPIED=$($_.Name):$($_.Length)" }
} finally {
    if ($HideAfter) {
        Remove-PartitionAccessPath -DiskNumber $DiskNumber -PartitionNumber 1 `
            -AccessPath "${letter}:\" -ErrorAction SilentlyContinue
    } else {
        "USB_BOOT_DRIVE=${letter}:\ (EFI partition type preserved)"
    }
}

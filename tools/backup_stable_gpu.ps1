[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$LogDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$image = Join-Path $root 'out\kestrelos-gputest.img'
$expected = 'B8D0411DE9C43092607753D12BEE41CAEA772092C453FBC820230E3D48EFA58F'
if ((Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash -ne $expected) {
    throw 'The user-confirmed stable image was replaced; refusing to mislabel a backup.'
}
$logPath = (Resolve-Path -LiteralPath $LogDirectory).Path
$destination = Join-Path $root ('backups\stable-gpu-20260908-' + (Get-Date -Format HHmmss))
if (Test-Path -LiteralPath $destination) { throw 'Backup already exists' }
New-Item -ItemType Directory -Path $destination | Out-Null
$trees = @('boot','common','data','docs','firmware','include','kernel','user','windows',
    'secureboot','tests','refs','third_party','Linux NVIDIA Driver','build',
    'out\nvidia-open-595.99.02')
foreach ($tree in $trees) {
    $src = Join-Path $root $tree
    if (-not (Test-Path -LiteralPath $src)) { continue }
    & robocopy $src (Join-Path $destination $tree) /E /COPY:DAT /DCOPY:DAT /R:1 /W:1 /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Backup failed for $tree" }
}
& robocopy (Join-Path $root 'tools') (Join-Path $destination 'tools') /E /COPY:DAT /DCOPY:DAT /R:1 /W:1 /XJ /XD (Join-Path $root 'tools\third_party') /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'Tools backup failed' }
Get-ChildItem -LiteralPath $root -File | Copy-Item -Destination $destination
foreach ($file in @('kestrelos-gputest.img','gputest-kernel.elf','gputest-initrd.kar',
    'gputest-loader.efi','flash-gputest-elevated.log','verify_gputest_usb.log')) {
    Copy-Item -LiteralPath (Join-Path $root "out\$file") -Destination (Join-Path $destination 'out')
}
Copy-Item -LiteralPath $logPath -Destination (Join-Path $destination 'confirmed-boot') -Recurse
$manifest = @()
foreach ($file in (Get-ChildItem -LiteralPath $destination -File -Recurse)) {
    $relative = $file.FullName.Substring($destination.Length + 1)
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    $original = if ($relative.StartsWith('confirmed-boot\')) {
        Join-Path $logPath $relative.Substring('confirmed-boot\'.Length)
    } else { Join-Path $root $relative }
    if ((Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash -ne $hash) {
        throw "Backup verification failed: $relative"
    }
    $manifest += [pscustomobject]@{ Path=$relative; Bytes=$file.Length; SHA256=$hash }
}
$manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $destination 'SHA256-MANIFEST.json') -Encoding UTF8
@"
User-confirmed stable GPU boot backup, created $(Get-Date -Format o).
Image SHA256: $expected
All copied files were hashed and compared with their source.
Evidence: confirmed-boot/KERNEL.LOG and gpu-boot.log.
Confirmed: all three heads 2560x1440 ~240Hz, 2D/3D, desktop startup.
Known limits: Acer misses initial red; CPU GUI composition with GPU presentation;
copy method buffer exhaustion during desktop; NVENC/NVDEC time out.
Includes source, build objects, firmware, NVIDIA build dependencies and exact image.
Optional external analysis tool installations (tools/third_party) are not included.
Restore the saved image directly; rebuilding is not needed to recover this boot.
tools/writeusb.ps1 accepts -Image; retain exact USB identity checks and UAC.
Do not run the hardcoded flash_gputest_usb wrapper to restore this backup: it selects
the current workspace image. Pass this backup's out/kestrelos-gputest.img explicitly.
"@ | Set-Content -LiteralPath (Join-Path $destination 'BACKUP-README.txt') -Encoding UTF8
Write-Output "BACKUP_VERIFIED=$destination"
Write-Output "FILES_VERIFIED=$($manifest.Count)"
exit 0

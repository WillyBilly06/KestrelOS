[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$InstanceId,
    [Parameter(Mandatory=$true)][uint32]$NvKmsDisplay
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$parts = $InstanceId -split '\\'
if ($parts.Count -ne 3 -or $parts[0] -ne 'DISPLAY') {
    throw "Expected a DISPLAY\\vendor\\instance device id"
}
$reg = "HKLM:\SYSTEM\CurrentControlSet\Enum\DISPLAY\$($parts[1])\$($parts[2])\Device Parameters"
$bytes = [byte[]](Get-ItemProperty -LiteralPath $reg -Name EDID).EDID
if ($bytes.Length -lt 128 -or ($bytes.Length % 128) -ne 0) {
    throw "EDID length $($bytes.Length) is not whole 128-byte blocks"
}
$magic = [byte[]](0,255,255,255,255,255,255,0)
for ($i=0; $i -lt 8; $i++) {
    if ($bytes[$i] -ne $magic[$i]) { throw 'EDID header is invalid' }
}
if (($bytes[126] + 1) -ne ($bytes.Length / 128)) {
    throw "EDID declares $($bytes[126]) extensions but contains $($bytes.Length / 128 - 1)"
}
for ($block=0; $block -lt $bytes.Length/128; $block++) {
    $sum = 0
    for ($i=0; $i -lt 128; $i++) {
        $sum = ($sum + $bytes[$block*128+$i]) -band 255
    }
    if ($sum -ne 0) { throw "EDID block $block checksum is nonzero" }
}
$dir = Join-Path $root 'firmware\edid'
New-Item -ItemType Directory -Path $dir -Force | Out-Null

# Preserve exactly what Windows reported before applying any narrowly-scoped
# compatibility repair.  Acer X27U firmware has been observed returning a
# checksum-correct but logically impossible EDID 1.4 range descriptor.  The
# NVIDIA 595 NVKMS strong validator rejects the *entire* EDID in that case,
# leaving only its 1024x768 VESA fallback.
$raw = [byte[]]$bytes.Clone()
$rawHash = [BitConverter]::ToString(
    [Security.Cryptography.SHA256]::Create().ComputeHash($raw)
).Replace('-', '').ToLowerInvariant()
$captureDir = Join-Path $dir 'captures'
New-Item -ItemType Directory -Path $captureDir -Force | Out-Null
$safeInstance = ($parts[1] + '-' + $parts[2]) -replace '[^A-Za-z0-9_.-]', '_'
$rawOut = Join-Path $captureDir ($safeInstance + '-' + $rawHash.Substring(0,16) + '.bin')
[IO.File]::WriteAllBytes($rawOut, $raw)

# Mirror NvTiming_EDIDValidationMask() from NVIDIA 595.99.02 for EDID 1.4
# range-limit descriptors.  Repair only a minimum-offset bit that makes min
# exceed max while the raw byte values themselves are ordered.  This cannot
# turn an unknown timing into a guessed timing; it removes a self-contradictory
# +255 flag and leaves every detailed/CTA/DisplayID timing byte untouched.
$repairs = @()
if ($bytes[18] -eq 1 -and $bytes[19] -eq 4 -and (($bytes[24] -band 1) -ne 0)) {
    for ($descriptor=0; $descriptor -lt 4; $descriptor++) {
        $off = 54 + 18*$descriptor
        if ($bytes[$off] -ne 0 -or $bytes[$off+1] -ne 0 -or
            $bytes[$off+2] -ne 0 -or $bytes[$off+3] -ne 0xFD) { continue }
        $flags = [int]$bytes[$off+4]
        if (($flags -band 0xF0) -ne 0) {
            throw ('EDID range descriptor has reserved flag bits set: 0x{0:X2}' -f $flags)
        }
        $minV = [int]$bytes[$off+5] + $(if ($flags -band 0x01) { 255 } else { 0 })
        $maxV = [int]$bytes[$off+6] + $(if ($flags -band 0x02) { 255 } else { 0 })
        $minH = [int]$bytes[$off+7] + $(if ($flags -band 0x04) { 255 } else { 0 })
        $maxH = [int]$bytes[$off+8] + $(if ($flags -band 0x08) { 255 } else { 0 })
        if ($minV -gt $maxV -and $bytes[$off+5] -le $bytes[$off+6] -and
            ($flags -band 0x01)) {
            $flags = $flags -band 0x0E
            $repairs += ('vertical range {0}>{1}: cleared erroneous +255 minimum flag' -f $minV,$maxV)
        }
        if ($minH -gt $maxH -and $bytes[$off+7] -le $bytes[$off+8] -and
            ($flags -band 0x04)) {
            $flags = $flags -band 0x0B
            $repairs += ('horizontal range {0}>{1}: cleared erroneous +255 minimum flag' -f $minH,$maxH)
        }
        $bytes[$off+4] = [byte]$flags
    }
}
if ($repairs.Count) {
    # Recompute the checksum of only the changed 128-byte base block.
    $bytes[127] = 0
    $sum = 0
    for ($i=0; $i -lt 127; $i++) { $sum = ($sum + $bytes[$i]) -band 255 }
    $bytes[127] = [byte]((- $sum) -band 255)
    $check = 0
    for ($i=0; $i -lt 128; $i++) { $check = ($check + $bytes[$i]) -band 255 }
    if ($check -ne 0) { throw 'internal error recomputing repaired base-block checksum' }
}

$name = 'nvkms-{0:x8}.bin' -f $NvKmsDisplay
$out = Join-Path $dir $name
[IO.File]::WriteAllBytes($out, $bytes)
# Binding is explicit. A saved asset filename alone must never authorize a
# zero-identity replacement sink on the former connector after a cable move.
$bindingsPath = Join-Path $dir 'nvkms-bindings.txt'
$bindings = @()
$key = '{0:x8}' -f $NvKmsDisplay
if (Test-Path -LiteralPath $bindingsPath) {
    foreach ($line in [IO.File]::ReadAllLines($bindingsPath)) {
        if (-not $line.Trim()) { continue }
        if ($line -notmatch '^[0-9a-fA-F]{8} [0-9a-fA-F]{8}$') { throw 'Malformed existing EDID bindings' }
        if ($line.Substring(0,8) -ne $key) { $bindings += $line }
    }
}
$bindings += "$key $key"
[IO.File]::WriteAllLines($bindingsPath, $bindings, [Text.Encoding]::ASCII)
$hash = (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash
Write-Host "Captured $($bytes.Length)-byte checksum-valid EDID from $InstanceId"
Write-Host "Raw capture preserved: $rawOut"
foreach ($repair in $repairs) { Write-Host "NVIDIA compatibility repair: $repair" }
Write-Host "NVKMS connector override: $out"
Write-Host "SHA-256: $hash"

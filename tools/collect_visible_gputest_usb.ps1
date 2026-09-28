$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Start-Transcript -LiteralPath (Join-Path $root 'out\collect-visible-usb.log') -Force | Out-Null
try {
    & (Join-Path $PSScriptRoot 'collect_gputest_usb_logs.ps1') -DiskNumber 3
    Write-Host 'COLLECT_VISIBLE_PASS'
} finally {
    Stop-Transcript | Out-Null
}

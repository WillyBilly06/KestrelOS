# ntfscheck.ps1 - let Windows judge a volume this system wrote to.
#
# Everything else in the NTFS tests is this project checking its own work.  The
# reader agreeing with the writer proves they were written by the same person,
# not that the volume is right - and "right" here means one specific thing:
# that Windows, whose format it is, mounts it, finds the files, and reports no
# damage.
#
# So this attaches the image and asks chkdsk.  Nothing else in the build can
# answer that question, and no amount of internal testing substitutes for it.
#
# Needs elevation, because attaching a virtual disk does.
#
#     powershell -ExecutionPolicy Bypass -File tools\ntfscheck.ps1 <image.vhd>

param(
    [Parameter(Mandatory = $true)][string]$Image
)

$ErrorActionPreference = 'Stop'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "This needs to run elevated - attaching a virtual disk does." -ForegroundColor Yellow
    exit 2
}

$Image = (Resolve-Path $Image).Path
Write-Host "checking $Image"

$disk = $null
try {
    $disk = Mount-DiskImage -ImagePath $Image -PassThru -NoDriveLetter:$false
    Start-Sleep -Milliseconds 500

    $vols = Get-DiskImage -ImagePath $Image | Get-Disk | Get-Partition |
            Get-Volume | Where-Object { $_.DriveLetter }

    if (-not $vols) {
        Write-Host "RESULT: Windows attached the disk but would not mount a volume." -ForegroundColor Red
        Write-Host "        That by itself is a failure - the filesystem is not readable."
        exit 1
    }

    foreach ($v in $vols) {
        $letter = $v.DriveLetter
        Write-Host ""
        Write-Host "volume ${letter}: filesystem=$($v.FileSystem) label=$($v.FileSystemLabel)"

        # What Windows can see. If the index entries were sorted the way this
        # driver believes, these are the files it wrote; if they were not, the
        # files are on the disk and this listing does not show them.
        Write-Host ""
        Write-Host "  what Windows lists under \KESTRELOS:"
        $seen = Get-ChildItem "${letter}:\KESTRELOS" -Force -ErrorAction SilentlyContinue
        if ($seen) {
            $seen | ForEach-Object {
                Write-Host ("    {0,-20} {1,10}" -f $_.Name, $(if ($_.PSIsContainer) { "<dir>" } else { $_.Length }))
            }
        } else {
            Write-Host "    nothing - either it was not created, or it was sorted where Windows will not look" -ForegroundColor Red
        }

        # And the verdict that matters.
        Write-Host ""
        Write-Host "  chkdsk:"
        $out = & chkdsk "${letter}:" 2>&1
        $out | Select-Object -Last 25 | ForEach-Object { Write-Host "    $_" }

        if ($LASTEXITCODE -eq 0) {
            Write-Host ""
            Write-Host "RESULT: Windows found no problems on ${letter}:." -ForegroundColor Green
        } else {
            Write-Host ""
            Write-Host "RESULT: chkdsk exited $LASTEXITCODE on ${letter}: - it found something." -ForegroundColor Red
        }
    }
}
finally {
    if ($disk) {
        Dismount-DiskImage -ImagePath $Image | Out-Null
        Write-Host ""
        Write-Host "detached."
    }
}

# genfont.ps1 - render a bitmap font (CP437 layout, 256 glyphs) into a C header.
#
# Text glyphs are rasterised from a TrueType console font via GDI+ with grid-fit
# hinting.  At the console's 8x16 size the line-drawing and block glyphs
# (0xB0-0xDF, 0xFE) are synthesised instead, because GDI rasterisation of those
# at that size produces broken, disconnected strokes that will not tile.
#
# Line-drawing model
# ------------------
#   A cell has four arms (up, down, left, right); each is 0 (absent),
#   1 (single) or 2 (double).  Single vertical rules occupy column 3, double
#   rules columns 2 and 4.  Single horizontal rules occupy row 7, double rules
#   rows 6 and 8.  A stroke with a matching arm opposite spans the whole cell;
#   otherwise it stops on one of the perpendicular rules - the near one when it
#   is a corner's inner stroke, the far one when it is the outer stroke or when
#   the perpendicular rule tees straight through.  A stroke that does span the
#   cell is then cut inside the opposite channel when that channel opens on the
#   stroke's own side, which is what turns a solid crossing into the four
#   separate elbows of a glyph like 0xCE.
#
# Usage:
#   .\genfont.ps1                                       # the 8x16 console font
#   .\genfont.ps1 -Width 12 -Height 24 -FontPx 19 -TextOnly -Symbol font_ui `
#                 -Out ..\user\libgui\font_ui.h
param(
    [string]$FontName = "Lucida Console",
    [int]$FontPx = 13,
    [int]$XOff = 0,
    [int]$YOff = 2,
    [int]$Width = 8,
    [int]$Height = 16,
    [string]$Symbol = "font8x16",
    [string]$Out = "",
    [switch]$Preview,
    [switch]$TextOnly,
    [switch]$Proportional,
    [string]$PreviewChars = ""
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

if (-not $Out) { $Out = Join-Path $PSScriptRoot "..\kernel\font8x16.h" }

$GW = $Width
$GH = $Height
$BPR = [int][math]::Ceiling($GW / 8)      # bytes per glyph row

# The synthesis below assumes an 8x16 cell; at any other size every glyph comes
# from the font.
$Synth = (-not $TextOnly) -and ($GW -eq 8) -and ($GH -eq 16)

# CP437 code point -> Unicode scalar.
$cp437 = @(
  0x0000,0x263A,0x263B,0x2665,0x2666,0x2663,0x2660,0x2022,0x25D8,0x25CB,0x25D9,0x2642,0x2640,0x266A,0x266B,0x263C,
  0x25BA,0x25C4,0x2195,0x203C,0x00B6,0x00A7,0x25AC,0x21A8,0x2191,0x2193,0x2192,0x2190,0x221F,0x2194,0x25B2,0x25BC,
  0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,
  0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
  0x0040,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,
  0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,0x0058,0x0059,0x005A,0x005B,0x005C,0x005D,0x005E,0x005F,
  0x0060,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
  0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,0x2302,
  0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
  0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
  0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
  0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
  0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
  0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
  0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
  0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0
)

# code -> @(up, down, left, right), each 0 = absent, 1 = single, 2 = double.
$BoxSpec = @{
    0xB3 = @(1,1,0,0); 0xB4 = @(1,1,1,0); 0xB5 = @(1,1,2,0); 0xB6 = @(2,2,1,0)
    0xB7 = @(0,2,1,0); 0xB8 = @(0,1,2,0); 0xB9 = @(2,2,2,0); 0xBA = @(2,2,0,0)
    0xBB = @(0,2,2,0); 0xBC = @(2,0,2,0); 0xBD = @(2,0,1,0); 0xBE = @(1,0,2,0)
    0xBF = @(0,1,1,0); 0xC0 = @(1,0,0,1); 0xC1 = @(1,0,1,1); 0xC2 = @(0,1,1,1)
    0xC3 = @(1,1,0,1); 0xC4 = @(0,0,1,1); 0xC5 = @(1,1,1,1); 0xC6 = @(1,1,0,2)
    0xC7 = @(2,2,0,1); 0xC8 = @(2,0,0,2); 0xC9 = @(0,2,0,2); 0xCA = @(2,0,2,2)
    0xCB = @(0,2,2,2); 0xCC = @(2,2,0,2); 0xCD = @(0,0,2,2); 0xCE = @(2,2,2,2)
    0xCF = @(1,0,2,2); 0xD0 = @(2,0,1,1); 0xD1 = @(0,1,2,2); 0xD2 = @(0,2,1,1)
    0xD3 = @(2,0,0,1); 0xD4 = @(1,0,0,2); 0xD5 = @(0,1,0,2); 0xD6 = @(0,2,0,1)
    0xD7 = @(2,2,1,1); 0xD8 = @(1,1,2,2); 0xD9 = @(1,0,1,0); 0xDA = @(0,1,0,1)
}

$script:PX = New-Object 'bool[]' ([int]($GW * $GH))

function Reset-Px { for ($i = 0; $i -lt $script:PX.Length; $i++) { $script:PX[$i] = $false } }
function Add-Px([int]$x, [int]$y) {
    if ($x -ge 0 -and $x -lt $GW -and $y -ge 0 -and $y -lt $GH) { $script:PX[$y * $GW + $x] = $true }
}
function Remove-Px([int]$x, [int]$y) {
    if ($x -ge 0 -and $x -lt $GW -and $y -ge 0 -and $y -lt $GH) { $script:PX[$y * $GW + $x] = $false }
}
function Add-HSeg([int]$y, [int]$x0, [int]$x1) { for ($x = $x0; $x -le $x1; $x++) { Add-Px $x $y } }
function Add-VSeg([int]$x, [int]$y0, [int]$y1) { for ($y = $y0; $y -le $y1; $y++) { Add-Px $x $y } }

function Get-PxRows {
    # Each row is ceil(width/8) bytes; the most significant bit is leftmost.
    $rows = New-Object 'byte[]' ([int]($GH * $BPR))
    for ($y = 0; $y -lt $GH; $y++) {
        for ($x = 0; $x -lt $GW; $x++) {
            if (-not $script:PX[$y * $GW + $x]) { continue }
            $index = [int]($y * $BPR + [math]::Floor($x / 8))
            $rows[$index] = $rows[$index] -bor (0x80 -shr ($x % 8))
        }
    }
    return ,$rows
}

function New-EmptyGlyph { return ,(New-Object 'byte[]' ([int]($GH * $BPR))) }

function Get-LineGlyph([int]$code) {
    Reset-Px
    $s = $BoxSpec[$code]
    $u = $s[0]; $d = $s[1]; $l = $s[2]; $r = $s[3]

    if     ($u -eq 2 -or $d -eq 2) { $vcols = @(2, 4) }
    elseif ($u -eq 1 -or $d -eq 1) { $vcols = @(3) }
    else                           { $vcols = @() }
    if     ($l -eq 2 -or $r -eq 2) { $hrows = @(6, 8) }
    elseif ($l -eq 1 -or $r -eq 1) { $hrows = @(7) }
    else                           { $hrows = @() }

    $minV = if ($vcols.Count) { $vcols[0] } else { 0 }
    $maxV = if ($vcols.Count) { $vcols[$vcols.Count - 1] } else { 0 }
    $minH = if ($hrows.Count) { $hrows[0] } else { 0 }
    $maxH = if ($hrows.Count) { $hrows[$hrows.Count - 1] } else { 0 }

    # Vertical rules.
    if ($u -gt 0 -and $d -gt 0) {
        foreach ($c in $vcols) { Add-VSeg $c 0 ($GH - 1) }
    } elseif ($u -gt 0) {
        foreach ($c in $vcols) {
            if ($hrows.Count -eq 0)          { $ye = $GH - 1 }
            elseif ($l -gt 0 -and $r -gt 0)  { $ye = $maxH }
            elseif ($vcols.Count -eq 1)      { $ye = $maxH }
            elseif ($l -gt 0)                { $ye = if ($c -eq $minV) { $minH } else { $maxH } }
            else                             { $ye = if ($c -eq $maxV) { $minH } else { $maxH } }
            Add-VSeg $c 0 $ye
        }
    } elseif ($d -gt 0) {
        foreach ($c in $vcols) {
            if ($hrows.Count -eq 0)          { $ys = 0 }
            elseif ($l -gt 0 -and $r -gt 0)  { $ys = $minH }
            elseif ($vcols.Count -eq 1)      { $ys = $minH }
            elseif ($l -gt 0)                { $ys = if ($c -eq $minV) { $maxH } else { $minH } }
            else                             { $ys = if ($c -eq $maxV) { $maxH } else { $minH } }
            Add-VSeg $c $ys ($GH - 1)
        }
    }

    # Horizontal rules.
    if ($l -gt 0 -and $r -gt 0) {
        foreach ($rw in $hrows) { Add-HSeg $rw 0 ($GW - 1) }
    } elseif ($l -gt 0) {
        foreach ($rw in $hrows) {
            if ($vcols.Count -eq 0)          { $xe = $GW - 1 }
            elseif ($u -gt 0 -and $d -gt 0)  { $xe = $minV }
            elseif ($hrows.Count -eq 1)      { $xe = $maxV }
            elseif ($u -gt 0)                { $xe = if ($rw -eq $minH) { $minV } else { $maxV } }
            else                             { $xe = if ($rw -eq $maxH) { $minV } else { $maxV } }
            Add-HSeg $rw 0 $xe
        }
    } elseif ($r -gt 0) {
        foreach ($rw in $hrows) {
            if ($vcols.Count -eq 0)          { $xs = 0 }
            elseif ($u -gt 0 -and $d -gt 0)  { $xs = $maxV }
            elseif ($hrows.Count -eq 1)      { $xs = $minV }
            elseif ($u -gt 0)                { $xs = if ($rw -eq $minH) { $maxV } else { $minV } }
            else                             { $xs = if ($rw -eq $maxH) { $maxV } else { $minV } }
            Add-HSeg $rw $xs ($GW - 1)
        }
    }

    # Open the channels: a rule spanning the cell is cut where the perpendicular
    # corridor opens on that rule's own side.
    if ($vcols.Count -eq 2) {
        foreach ($rw in $hrows) {
            if ($rw -eq 6)     { $cut = ($u -gt 0) }
            elseif ($rw -eq 8) { $cut = ($d -gt 0) }
            else               { $cut = ($u -gt 0 -and $d -gt 0) }
            if ($cut) { for ($x = $minV + 1; $x -lt $maxV; $x++) { Remove-Px $x $rw } }
        }
    }
    if ($hrows.Count -eq 2) {
        foreach ($c in $vcols) {
            if ($c -eq 2)     { $cut = ($l -gt 0) }
            elseif ($c -eq 4) { $cut = ($r -gt 0) }
            else              { $cut = ($l -gt 0 -and $r -gt 0) }
            if ($cut) { for ($y = $minH + 1; $y -lt $maxH; $y++) { Remove-Px $c $y } }
        }
    }
    return Get-PxRows
}

function Get-BlockGlyph([int]$code) {
    Reset-Px
    $halfW = [int]($GW / 2)
    $halfH = [int]($GH / 2)
    switch ($code) {
        0xB0 { for ($y = 0; $y -lt $GH; $y++) { for ($x = 0; $x -lt $GW; $x++) { if (($x % 4) -eq (($y % 2) * 2)) { Add-Px $x $y } } } }
        0xB1 { for ($y = 0; $y -lt $GH; $y++) { for ($x = 0; $x -lt $GW; $x++) { if ((($x + $y) % 2) -eq 0) { Add-Px $x $y } } } }
        0xB2 { for ($y = 0; $y -lt $GH; $y++) { for ($x = 0; $x -lt $GW; $x++) { if (($x % 4) -ne (($y % 2) * 2)) { Add-Px $x $y } } } }
        0xDB { for ($y = 0; $y -lt $GH; $y++) { Add-HSeg $y 0 ($GW - 1) } }
        0xDC { for ($y = $halfH; $y -lt $GH; $y++) { Add-HSeg $y 0 ($GW - 1) } }
        0xDD { for ($y = 0; $y -lt $GH; $y++) { Add-HSeg $y 0 ($halfW - 1) } }
        0xDE { for ($y = 0; $y -lt $GH; $y++) { Add-HSeg $y $halfW ($GW - 1) } }
        0xDF { for ($y = 0; $y -lt $halfH; $y++) { Add-HSeg $y 0 ($GW - 1) } }
        0xFE { for ($y = 5; $y -le 10; $y++)  { Add-HSeg $y 1 6 } }
    }
    return Get-PxRows
}

$font = New-Object System.Drawing.Font($FontName, $FontPx, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$fmt = [System.Drawing.StringFormat]::GenericTypographic

$script:Advance = New-Object 'int[]' 256

function Get-TextGlyph([int]$cp, [int]$index) {
    $bmp = New-Object System.Drawing.Bitmap($GW, $GH)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Black)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::SingleBitPerPixelGridFit
    $g.DrawString([string][char]$cp, $font, [System.Drawing.Brushes]::White, [single]$XOff, [single]$YOff, $fmt)
    $g.Dispose()
    Reset-Px
    $inkRight = -1
    for ($y = 0; $y -lt $GH; $y++) {
        for ($x = 0; $x -lt $GW; $x++) {
            if ($bmp.GetPixel($x, $y).R -le 127) { continue }
            Add-Px $x $y
            if ($x -gt $inkRight) { $inkRight = $x }
        }
    }
    $bmp.Dispose()

    # A proportional font advances by the glyph's own width plus one column of
    # spacing; a blank glyph still needs a sensible width so spaces work.
    if ($Proportional) {
        if ($inkRight -lt 0) { $script:Advance[$index] = [int][math]::Max(3, [math]::Round($GW / 3)) }
        else { $script:Advance[$index] = [int][math]::Min($GW, $inkRight + 2) }
    } else {
        $script:Advance[$index] = $GW
    }
    return Get-PxRows
}

$glyphs = New-Object 'object[]' 256
for ($i = 0; $i -lt 256; $i++) {
    $script:Advance[$i] = $GW
    if ($Synth -and $BoxSpec.ContainsKey($i)) { $glyphs[$i] = Get-LineGlyph $i }
    elseif ($Synth -and ((($i -ge 0xB0 -and $i -le 0xB2) -or ($i -ge 0xDB -and $i -le 0xDF) -or $i -eq 0xFE))) {
        $glyphs[$i] = Get-BlockGlyph $i
    }
    elseif ($i -eq 0x00 -or $i -eq 0x20 -or $i -eq 0xFF) {
        $glyphs[$i] = New-EmptyGlyph
        if ($Proportional -and $i -eq 0x20) { $script:Advance[$i] = [int][math]::Max(3, [math]::Round($GW / 3)) }
    }
    else { $glyphs[$i] = Get-TextGlyph $cp437[$i] $i }
}
$font.Dispose()

if ($Preview) {
    if ($PreviewChars) { $codes = $PreviewChars -split ',' | ForEach-Object { [Convert]::ToInt32($_.Trim(), 16) } }
    else { $codes = @(0x41, 0x67, 0x4D, 0x40, 0xC5, 0xCE, 0xCB, 0xCA, 0xC9, 0xBC, 0xB9, 0xD7, 0xD8, 0xDB, 0xB1) }
    $prev = New-Object System.Text.StringBuilder
    foreach ($c in $codes) {
        [void]$prev.AppendLine(("--- 0x{0:X2} ---" -f $c))
        $rows = $glyphs[$c]
        for ($y = 0; $y -lt $GH; $y++) {
            $line = ""
            for ($x = 0; $x -lt $GW; $x++) {
                $b = $rows[[int]($y * $BPR + [math]::Floor($x / 8))]
                $line += if (($b -band (0x80 -shr ($x % 8))) -ne 0) { "#" } else { "." }
            }
            [void]$prev.AppendLine($line)
        }
    }
    Write-Host $prev.ToString()
}

$upper = $Symbol.ToUpper()
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("/* Generated by tools/genfont.ps1 - do not edit by hand.")
[void]$sb.AppendLine((" * Text glyphs: {0} {1}px (offset {2},{3}).{4}" -f $FontName, $FontPx, $XOff, $YOff,
                      $(if ($Synth) { " Line and block glyphs: synthesised." } else { "" })))
[void]$sb.AppendLine((" * 256 glyphs, CP437 layout, {0}x{1}, {2} byte(s) per row, MSB leftmost. */" -f $GW, $GH, $BPR))
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("#include <stdint.h>")
[void]$sb.AppendLine("")
[void]$sb.AppendLine(("#define {0}_W {1}" -f $upper, $GW))
[void]$sb.AppendLine(("#define {0}_H {1}" -f $upper, $GH))
[void]$sb.AppendLine(("#define {0}_STRIDE {1}" -f $upper, $BPR))
[void]$sb.AppendLine(("#define {0}_PROPORTIONAL {1}" -f $upper, $(if ($Proportional) { 1 } else { 0 })))
[void]$sb.AppendLine("")
[void]$sb.AppendLine(("static const uint8_t {0}[256][{1}] = {{" -f $Symbol, [int]($GH * $BPR)))
for ($i = 0; $i -lt 256; $i++) {
    $hex = (($glyphs[$i]) | ForEach-Object { "0x{0:X2}" -f $_ }) -join ","
    [void]$sb.AppendLine(("    {{{0}}}, /* 0x{1:X2} */" -f $hex, $i))
}
[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
if ($Proportional) {
    [void]$sb.AppendLine("/* Advance width of each glyph, in pixels. */")
} else {
    [void]$sb.AppendLine("/* Every glyph advances by the full cell width. */")
}
[void]$sb.AppendLine(("static const uint8_t {0}_width[256] = {{" -f $Symbol))
for ($i = 0; $i -lt 256; $i += 16) {
    $row = @()
    for ($j = 0; $j -lt 16; $j++) { $row += ("{0,2}" -f $script:Advance[$i + $j]) }
    [void]$sb.AppendLine("    " + ($row -join ",") + ",")
}
[void]$sb.AppendLine("};")

$dir = Split-Path -Parent $Out
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$full = Join-Path (Resolve-Path -LiteralPath $dir).Path (Split-Path -Leaf $Out)
[System.IO.File]::WriteAllText($full, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("Wrote {0}  ({1}x{2}, symbol {3})" -f $full, $GW, $GH, $Symbol)

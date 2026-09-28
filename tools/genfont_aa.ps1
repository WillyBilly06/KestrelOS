# genfont_aa.ps1 - render an anti-aliased font into a C header.
#
# The console font next door is one bit per pixel, which is right for a
# terminal: at 8x16 a grid-fitted bitmap is sharper than anything anti-aliasing
# would produce, and every glyph lands on the same grid.
#
# A page of text is a different problem.  Headings want to be large, body text
# wants to be small, and both want the shapes to be right rather than aligned
# to a grid - a hard-edged letter at twenty-six pixels looks like a screenshot
# from 1994.  So these glyphs keep a coverage value per pixel: how much of that
# pixel the letter covers, from nothing to completely.  Drawing then blends the
# text colour into the background by that amount, and the curves come out
# smooth.
#
# The cost is eight times the storage of a one-bit font.  For the handful of
# faces a browser needs, that is a fair trade; for the console it would not be.
#
# Usage:
#   .\genfont_aa.ps1 -FontName "Segoe UI" -FontPx 16 -Width 16 -Height 21 `
#                    -Symbol font_web -Out ..\user\libgui\font_web.h
param(
    [string]$FontName = "Segoe UI",
    [int]$FontPx = 16,
    [string]$Style = "Regular",         # Regular, Bold, Italic, BoldItalic
    [int]$XOff = 0,
    [int]$YOff = 0,
    [int]$Width = 16,
    [int]$Height = 21,
    [int]$Baseline = -1,                # rows from the top to the baseline
    [string]$Symbol = "font_web",
    [string]$Out = "",
    [switch]$Preview
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

if (-not $Out) { $Out = Join-Path $PSScriptRoot ("..\user\libgui\" + $Symbol + ".h") }

$GW = $Width
$GH = $Height

# CP437 code point -> Unicode scalar, the same layout the rest of the system
# uses so that one string can be drawn in any of the fonts.
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

$fontStyle = switch ($Style) {
    "Bold"       { [System.Drawing.FontStyle]::Bold }
    "Italic"     { [System.Drawing.FontStyle]::Italic }
    "BoldItalic" { [System.Drawing.FontStyle]::Bold -bor [System.Drawing.FontStyle]::Italic }
    default      { [System.Drawing.FontStyle]::Regular }
}
$font = New-Object System.Drawing.Font($FontName, $FontPx, $fontStyle, [System.Drawing.GraphicsUnit]::Pixel)
$fmt = [System.Drawing.StringFormat]::GenericTypographic

# Rendering at four times the size and averaging down gives coverage values
# that reflect the outline, rather than trusting the system's own anti-aliasing
# - which is tuned for a particular screen and bakes assumptions about subpixel
# layout into what should be a plain coverage number.
$SS = 4

$script:Advance = New-Object 'int[]' 256
$glyphs = New-Object 'object[]' 256

function Get-Glyph([int]$cp, [int]$index) {
    $bw = $GW * $SS
    $bh = $GH * $SS
    $bmp = New-Object System.Drawing.Bitmap($bw, $bh)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Black)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.ScaleTransform([single]$SS, [single]$SS)
    $g.DrawString([string][char]$cp, $font, [System.Drawing.Brushes]::White,
                  [single]$XOff, [single]$YOff, $fmt)
    $g.Dispose()

    # Read the whole bitmap once: GetPixel on a large image one call at a time
    # is slow enough to matter across two hundred and fifty-six glyphs.
    $rect = New-Object System.Drawing.Rectangle(0, 0, $bw, $bh)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $bytes = New-Object 'byte[]' ($data.Stride * $bh)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $stride = $data.Stride
    $bmp.UnlockBits($data)
    $bmp.Dispose()

    $out = New-Object 'byte[]' ($GW * $GH)
    $inkRight = -1
    $area = $SS * $SS

    for ($y = 0; $y -lt $GH; $y++) {
        for ($x = 0; $x -lt $GW; $x++) {
            $sum = 0
            for ($sy = 0; $sy -lt $SS; $sy++) {
                $row = ($y * $SS + $sy) * $stride
                for ($sx = 0; $sx -lt $SS; $sx++) {
                    $sum += $bytes[$row + ($x * $SS + $sx) * 4 + 2]   # the red channel
                }
            }
            $value = [int][math]::Round($sum / $area)
            if ($value -gt 255) { $value = 255 }
            $out[$y * $GW + $x] = [byte]$value
            if ($value -ge 24 -and $x -gt $inkRight) { $inkRight = $x }
        }
    }

    # How far the pen moves after this letter.  Measuring the ink rather than
    # asking the font keeps the spacing consistent with what was actually
    # rasterised into the cell.
    if ($inkRight -lt 0) { $script:Advance[$index] = [int][math]::Max(3, [math]::Round($GW / 3.5)) }
    else { $script:Advance[$index] = [int][math]::Min($GW, $inkRight + 2) }

    return $out
}

for ($i = 0; $i -lt 256; $i++) {
    if ($i -eq 0x00 -or $i -eq 0xFF) {
        $glyphs[$i] = New-Object 'byte[]' ($GW * $GH)
        $script:Advance[$i] = [int][math]::Max(3, [math]::Round($GW / 3.5))
    } else {
        $glyphs[$i] = Get-Glyph $cp437[$i] $i
    }
}
$font.Dispose()

if ($Baseline -lt 0) { $Baseline = [int][math]::Round($GH * 0.78) }

if ($Preview) {
    $shades = " .:-=+*#%@"
    foreach ($c in @(0x41, 0x67, 0x53, 0x40)) {
        Write-Host ("--- 0x{0:X2} ---" -f $c)
        for ($y = 0; $y -lt $GH; $y++) {
            $line = ""
            for ($x = 0; $x -lt $GW; $x++) {
                $v = $glyphs[$c][$y * $GW + $x]
                $line += $shades[[int][math]::Min(9, [math]::Floor($v / 26))]
            }
            Write-Host $line
        }
    }
}

$upper = $Symbol.ToUpper()
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("/* Generated by tools/genfont_aa.ps1 - do not edit by hand.")
[void]$sb.AppendLine((" * {0} {1}px {2}, anti-aliased." -f $FontName, $FontPx, $Style))
[void]$sb.AppendLine((" * 256 glyphs, CP437 layout, {0}x{1}, one coverage byte per pixel. */" -f $GW, $GH))
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("#include <stdint.h>")
[void]$sb.AppendLine("")
[void]$sb.AppendLine(("#define {0}_W {1}" -f $upper, $GW))
[void]$sb.AppendLine(("#define {0}_H {1}" -f $upper, $GH))
[void]$sb.AppendLine(("#define {0}_BASELINE {1}" -f $upper, $Baseline))
[void]$sb.AppendLine("")
[void]$sb.AppendLine(("static const uint8_t {0}[256][{1}] = {{" -f $Symbol, [int]($GW * $GH)))
for ($i = 0; $i -lt 256; $i++) {
    $hex = (($glyphs[$i]) | ForEach-Object { "0x{0:X2}" -f $_ }) -join ","
    [void]$sb.AppendLine(("    {{{0}}}, /* 0x{1:X2} */" -f $hex, $i))
}
[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine(("static const uint8_t {0}_width[256] = {{" -f $Symbol))
for ($i = 0; $i -lt 256; $i += 16) {
    $row = @()
    for ($j = 0; $j -lt 16; $j++) { $row += ("{0,3}" -f $script:Advance[$i + $j]) }
    [void]$sb.AppendLine("    " + ($row -join ",") + ",")
}
[void]$sb.AppendLine("};")

$dir = Split-Path -Parent $Out
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$full = Join-Path (Resolve-Path -LiteralPath $dir).Path (Split-Path -Leaf $Out)
[System.IO.File]::WriteAllText($full, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("Wrote {0}  ({1}x{2}, symbol {3}, {4} bytes)" -f $full, $GW, $GH, $Symbol, (Get-Item $full).Length)

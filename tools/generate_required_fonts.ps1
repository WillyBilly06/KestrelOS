# Generate local glyph headers. Requires locally licensed Segoe UI, Consolas,
# and Lucida Console fonts. Generated headers are ignored by Git.
$ErrorActionPreference = 'Stop'
$toolsDir = $PSScriptRoot
$guiDir = Join-Path (Split-Path $toolsDir) 'user\libgui'

& (Join-Path $toolsDir 'genfont.ps1') -FontName 'Lucida Console' -FontPx 13 -XOff 0 -YOff 2 -Width 8 -Height 16 -Symbol 'font8x16' -Out (Join-Path (Split-Path $toolsDir) 'kernel\font8x16.h')
& (Join-Path $toolsDir 'genfont.ps1') -FontName 'Consolas' -FontPx 15 -XOff 0 -YOff 1 -Width 8 -Height 17 -TextOnly -Symbol 'font_mono' -Out (Join-Path $guiDir 'font_mono.h')
& (Join-Path $toolsDir 'genfont.ps1') -FontName 'Segoe UI' -FontPx 19 -XOff 0 -YOff 1 -Width 16 -Height 24 -TextOnly -Proportional -Symbol 'font_ui' -Out (Join-Path $guiDir 'font_ui.h')

$aa = Join-Path $toolsDir 'genfont_aa.ps1'
& $aa -FontName 'Segoe UI' -FontPx 19 -Width 16 -Height 24 -Symbol 'font_uiaa' -Out (Join-Path $guiDir 'font_uiaa.h')
& $aa -FontName 'Segoe UI' -FontPx 38 -Width 32 -Height 48 -Symbol 'font_ui2x' -Out (Join-Path $guiDir 'font_ui2x.h')
& $aa -FontName 'Segoe UI' -FontPx 57 -Width 48 -Height 72 -Symbol 'font_ui3x' -Out (Join-Path $guiDir 'font_ui3x.h')
& $aa -FontName 'Segoe UI' -FontPx 16 -Width 16 -Height 21 -Symbol 'font_web' -Out (Join-Path $guiDir 'font_web.h')
& $aa -FontName 'Segoe UI' -FontPx 16 -Width 16 -Height 21 -Style 'Bold' -Symbol 'font_web_bold' -Out (Join-Path $guiDir 'font_web_bold.h')
& $aa -FontName 'Segoe UI' -FontPx 32 -Width 32 -Height 42 -Style 'Bold' -Symbol 'font_web_head' -Out (Join-Path $guiDir 'font_web_head.h')
& $aa -FontName 'Consolas' -FontPx 15 -Width 10 -Height 20 -Symbol 'font_web_mono' -Out (Join-Path $guiDir 'font_web_mono.h')

# Generates SuperFbFontArtData.c: high-resolution (256px-tall cell) glyphs for
# the Pretentious Mode "Character" art, rendered from the same font file as
# the main UI font so the art matches the UI.
#
# The UI font stores 32px glyphs and the art mode used to upscale them, which
# blurred the characters.  The art glyphs here are full 256px cells (rendered
# at 512px and downsampled 2:1 for anti-aliasing), displayed 1:1 or at an
# integer scale with nearest-neighbour sampling so the art stays sharp.
#
#   powershell -ExecutionPolicy Bypass -File gen_sfb_font_art.ps1
#   powershell -ExecutionPolicy Bypass -File gen_sfb_font_art.ps1 -FontFile .\Roboto-Regular.ttf
#
# The output file is written next to the UI font data; the C file is committed
# so the firmware build never needs to run this script.

param(
    # TTF/OTF to render from; defaults to Roboto-Regular.ttf next to this
    # script.
    [string]$FontFile = ''
)

$ErrorActionPreference = 'Stop'

$CellHeight   = 256
$Grid         = $CellHeight * 2           # 512x512 downsample source grid
$RenderSize   = 600                       # tall render canvas (no clipping)
$FontPx       = 512
$AlphaLevels  = 16
$CjkAdvance   = 256                       # uniform full-width cell

# Characters used by Pretentious Mode "Character" art (SuperFbMenu.c):
# 0x8C6A hao, 0x60C5 qing, 0x5728 zai, 0x5929 tian, 0x5609 jia, 0x6B23 xin
$ArtChars = -join [char[]](0x8C6A, 0x60C5, 0x5728, 0x5929, 0x5609, 0x6B23)

$ScriptDir = $PSScriptRoot
if ($FontFile -eq '') {
    $FontFile = Join-Path $ScriptDir 'Roboto-Regular.ttf'
}
$OutFile = [System.IO.Path]::GetFullPath(
    (Join-Path $ScriptDir '..\..\submodules\uefi\edk2\QcomModulePkg\Application\LinuxLoader\SuperFbFontArtData.c'))

Add-Type -AssemblyName System.Drawing

$ResolvedFontFile = (Resolve-Path $FontFile -ErrorAction Stop).Path
$PrivateFonts = New-Object System.Drawing.Text.PrivateFontCollection
$PrivateFonts.AddFontFile($ResolvedFontFile)
$FontFamily = $PrivateFonts.Families[0]
$FontSource = (Split-Path -Leaf $ResolvedFontFile) + ' (' + $FontFamily.Name + ')'
Write-Host ("Using font file: {0}" -f $ResolvedFontFile)
Write-Host ("Font family: {0}" -f $FontFamily.Name)

# ---- render one character into a packed 4-bit art glyph ------------------
function Get-ArtGlyph([char]$Ch) {
    $bmp = New-Object System.Drawing.Bitmap -ArgumentList $RenderSize, $RenderSize, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)
    $font = New-Object System.Drawing.Font($FontFamily, $FontPx, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $fmt = [System.Drawing.StringFormat]::GenericTypographic
    $g.DrawString([string]$Ch, $font, [System.Drawing.Brushes]::White, (New-Object System.Drawing.PointF(0, 0)), $fmt)
    $font.Dispose(); $fmt.Dispose(); $g.Dispose()

    $rect = New-Object System.Drawing.Rectangle(0, 0, $RenderSize, $RenderSize)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $data.Stride
    $bytes = New-Object byte[] ($stride * $RenderSize)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data)
    $bmp.Dispose()

    $minX = $RenderSize; $minY = $RenderSize; $maxX = -1; $maxY = -1
    for ($y = 0; $y -lt $RenderSize; $y++) {
        for ($x = 0; $x -lt $RenderSize; $x++) {
            $a = $bytes[$y * $stride + $x * 4 + 3]
            if ($a -gt 8) {
                if ($x -lt $minX) { $minX = $x }
                if ($x -gt $maxX) { $maxX = $x }
                if ($y -lt $minY) { $minY = $y }
                if ($y -gt $maxY) { $maxY = $y }
            }
        }
    }

    if ($maxX -lt 0) {
        Write-Error ("Font is missing glyph for U+{0:X4}" -f [int]$Ch)
    }

    $inkW = $maxX - $minX + 1
    $h = $maxY - $minY + 1

    # Art glyphs are all full-width CJK: one uniform 256px cell, ink centred.
    $cellW = $CjkAdvance
    $off = [Math]::Max(0, [Math]::Floor(($cellW * 2 - $inkW) / 2))

    # Centre the ink vertically in the cell (all art glyphs are CJK, so there
    # is no Latin baseline to align against).  RenderTop is the render row
    # sampled by final row 0; ink rows map to (minY-RenderTop)/2 .. (maxY-RenderTop)/2.
    $FinalInkH = [Math]::Ceiling($h / 2.0)
    $FinalTop = [Math]::Floor(($CellHeight - $FinalInkH) / 2.0)
    $RenderTop = $minY - 2 * $FinalTop
    $pasteY = $minY - $RenderTop

    $InkTop = $minY - $RenderTop
    $InkBottom = $maxY - $RenderTop
    if ($InkTop -lt 0 -or $InkBottom -ge $Grid) {
        Write-Host ("WARNING: glyph U+{0:X4} does not fit the cell (y {1}..{2})" -f [int]$Ch, $InkTop, $InkBottom)
    }

    $out = New-Object byte[] (($cellW * $CellHeight) / 2)
    for ($y = 0; $y -lt $CellHeight; $y++) {
        for ($x = 0; $x -lt $cellW; $x++) {
            $sum = 0
            for ($jy = 0; $jy -lt 2; $jy++) {
                for ($jx = 0; $jx -lt 2; $jx++) {
                    $px = $minX - $off + $x * 2 + $jx
                    $py = $y * 2 + $jy - $pasteY + $minY
                    if ($px -ge 0 -and $px -lt $RenderSize -and $py -ge 0 -and $py -lt $RenderSize) {
                        $sum += $bytes[$py * $stride + $px * 4 + 3]
                    }
                }
            }
            $alpha = [Math]::Min($AlphaLevels - 1, [Math]::Floor($sum / 4 / 16))
            $idx = $y * $cellW + $x
            $byteIdx = [int][Math]::Floor($idx / 2)
            if (($idx % 2) -eq 0) {
                $out[$byteIdx] = $alpha -shl 4
            } else {
                $out[$byteIdx] = $out[$byteIdx] -bor $alpha
            }
        }
    }

    return @{ Bytes = $out; Width = $cellW; Advance = $CjkAdvance }
}

# ---- build the C source -----------------------------------------------------
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('/*')
[void]$sb.AppendLine(' * Generated by tools/gen_sfb_font/gen_sfb_font_art.ps1 - do not edit by hand.')
[void]$sb.AppendLine(" * Font: $FontSource, ${CellHeight}px tall art cells, 4-bit anti-aliased alpha.")
[void]$sb.AppendLine(' * High-resolution glyphs for Pretentious Mode character art.')
[void]$sb.AppendLine(' */')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#include "SuperFbFont.h"')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('CONST UINT8 gSfbArtBitmap[] = {')

$offset = 0
$glyphs = New-Object System.Collections.Generic.List[string]
$row = New-Object System.Collections.Generic.List[string]
foreach ($ch in $ArtChars.ToCharArray()) {
    $glyph = Get-ArtGlyph $ch
    [void]$glyphs.Add(('{{ 0x{0:X4}, {1}, {2}, {3} }}' -f [int]$ch, $offset, $glyph.Width, $glyph.Advance))
    $offset += $glyph.Bytes.Length
    for ($i = 0; $i -lt $glyph.Bytes.Length; $i++) {
        [void]$row.Add(('0x{0:X2}' -f $glyph.Bytes[$i]))
        if ($row.Count -eq 16) {
            [void]$sb.AppendLine('  ' + ($row -join ', ') + ',')
            $row.Clear()
        }
    }
}
if ($row.Count -gt 0) {
    [void]$sb.AppendLine('  ' + ($row -join ', ') + ',')
}
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('CONST UINTN gSfbArtGlyphCount = ' + $glyphs.Count + ';')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('CONST SFB_ART_GLYPH gSfbArtGlyphs[] = {')
foreach ($gl in $glyphs) {
    [void]$sb.AppendLine('  ' + $gl + ',')
}
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')

[System.IO.File]::WriteAllText($OutFile, $sb.ToString(), (New-Object System.Text.ASCIIEncoding))
Write-Host ("Wrote {0} art glyphs ({1} bytes bitmap) to {2}" -f $glyphs.Count, $offset, $OutFile)

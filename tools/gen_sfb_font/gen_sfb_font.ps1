# Generates SuperFbFontData.c from the UI strings in SuperFbLang.c.
#
# Renders every unique character used by the BDS UI with Noto Sans SC at 2x
# resolution, downsamples to a 24x24 cell and stores 4-bit anti-aliased alpha
# (2 pixels per byte).  Run on Windows with PowerShell 5.1+:
#
#   powershell -ExecutionPolicy Bypass -File gen_sfb_font.ps1
#
# The output file is written next to the input source; the C file is committed
# so the firmware build never needs to run this script.

$ErrorActionPreference = 'Stop'

$FontName      = 'Noto Sans SC'
$CellSize      = 24
$Grid          = $CellSize * 2          # 48x48 downsample source grid
$RenderSize    = 80                     # tall render canvas (no top clipping)
$FontPx        = 40
$AlphaLevels   = 16
$CellBaseline  = 36                     # baseline row in the 48 grid (cell row 18)

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$SrcFile = Join-Path $RepoRoot 'submodules\uefi\edk2\QcomModulePkg\Application\LinuxLoader\SuperFbLang.c'
$OutFile = Join-Path $RepoRoot 'submodules\uefi\edk2\QcomModulePkg\Application\LinuxLoader\SuperFbFontData.c'

if (!(Test-Path $SrcFile)) {
    Write-Error "Source strings file not found: $SrcFile"
}

Add-Type -AssemblyName System.Drawing

# ---- collect every character used in L"..." literals -----------------------
$text = [System.IO.File]::ReadAllText($SrcFile, [System.Text.Encoding]::UTF8)
$matches = [regex]::Matches($text, 'L"((?:[^"\\]|\\.)*)"')
if ($matches.Count -eq 0) {
    Write-Error 'No L"..." string literals found in SuperFbLang.c'
}

$chars = New-Object 'System.Collections.Generic.HashSet[char]'
foreach ($m in $matches) {
    $s = $m.Groups[1].Value
    for ($i = 0; $i -lt $s.Length; $i++) {
        $c = $s[$i]
        if ($c -eq '\') { continue }
        [void]$chars.Add($c)
    }
}

$sorted = @($chars) | Sort-Object
Write-Host ("Collected {0} unique characters" -f $sorted.Count)

# ---- measure the render-canvas baseline once ------------------------------
function Measure-Baseline {
    $bmp = New-Object System.Drawing.Bitmap -ArgumentList $RenderSize, $RenderSize, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)
    $font = New-Object System.Drawing.Font($FontName, $FontPx, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $fmt = [System.Drawing.StringFormat]::GenericTypographic
    # 'X' has no descender: its ink bottom sits on the baseline.
    $g.DrawString('X', $font, [System.Drawing.Brushes]::White, (New-Object System.Drawing.PointF(0, 0)), $fmt)
    $rect = New-Object System.Drawing.Rectangle(0, 0, $RenderSize, $RenderSize)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $bytes = New-Object byte[] ($data.Stride * $RenderSize)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data)
    $maxY = -1
    for ($y = 0; $y -lt $RenderSize; $y++) {
        for ($x = 0; $x -lt $RenderSize; $x++) {
            if ($bytes[$y * $data.Stride + $x * 4 + 3] -gt 8) { $maxY = $y }
        }
    }
    $font.Dispose(); $fmt.Dispose(); $g.Dispose(); $bmp.Dispose()
    return $maxY
}

$BaselineY = Measure-Baseline
if ($BaselineY -lt 0) { Write-Error 'Could not measure the font baseline' }
Write-Host ("Render baseline at y={0}, cell baseline row {1}" -f $BaselineY, ($CellBaseline / 2))

# ---- render one character into a packed 4-bit 24x24 glyph -----------------
function Get-GlyphBytes([char]$Ch) {
    $bmp = New-Object System.Drawing.Bitmap -ArgumentList $RenderSize, $RenderSize, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)
    $font = New-Object System.Drawing.Font($FontName, $FontPx, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $brush = [System.Drawing.Brushes]::White
    $fmt = [System.Drawing.StringFormat]::GenericTypographic
    $g.DrawString([string]$Ch, $font, $brush, (New-Object System.Drawing.PointF(0, 0)), $fmt)
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

    $out = New-Object byte[] (($CellSize * $CellSize) / 2)
    if ($maxX -lt 0) { return ,$out }

    $w = $maxX - $minX + 1
    $h = $maxY - $minY + 1
    $pasteX = [Math]::Floor(($Grid - $w) / 2)
    $pasteY = $CellBaseline - ($BaselineY - $minY)

    # The glyph must sit fully inside the 48 grid with its baseline on the
    # fixed baseline row; otherwise the cell would clip it.
    if ($pasteY -lt 0 -or $pasteY + $h -gt $Grid) {
        Write-Host ("WARNING: glyph U+{0:X4} does not fit the cell (y {1}..{2})" -f [int]$Ch, $pasteY, ($pasteY + $h - 1))
    }

    for ($y = 0; $y -lt $CellSize; $y++) {
        for ($x = 0; $x -lt $CellSize; $x++) {
            $sum = 0
            for ($jy = 0; $jy -lt 2; $jy++) {
                for ($jx = 0; $jx -lt 2; $jx++) {
                    # Map the 48-grid pixel back onto the render-canvas ink.
                    $px = $x * 2 + $jx - $pasteX + $minX
                    $py = $y * 2 + $jy - $pasteY + $minY
                    if ($px -ge 0 -and $px -lt $RenderSize -and $py -ge 0 -and $py -lt $RenderSize) {
                        $sum += $bytes[$py * $stride + $px * 4 + 3]
                    }
                }
            }
            $alpha = [Math]::Min($AlphaLevels - 1, [Math]::Floor($sum / 4 / 16))
            $idx = $y * $CellSize + $x
            $byteIdx = [int][Math]::Floor($idx / 2)
            if (($idx % 2) -eq 0) {
                $out[$byteIdx] = $alpha -shl 4
            } else {
                $out[$byteIdx] = $out[$byteIdx] -bor $alpha
            }
        }
    }
    return ,$out
}

# ---- build the C source -----------------------------------------------------
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('/*')
[void]$sb.AppendLine(' * Generated by tools/gen_sfb_font/gen_sfb_font.ps1 - do not edit by hand.')
[void]$sb.AppendLine(" * Font: $FontName, $CellSize x $CellSize cell, 4-bit anti-aliased alpha.")
[void]$sb.AppendLine(' */')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#include "SuperFbFont.h"')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('CONST UINT8 gSfbFontBitmap[] = {')

$offset = 0
$glyphs = New-Object System.Collections.Generic.List[string]
$row = New-Object System.Collections.Generic.List[string]
foreach ($ch in $sorted) {
    $glyph = Get-GlyphBytes $ch
    [void]$glyphs.Add(('{{ 0x{0:X4}, {1} }}' -f [int]$ch, $offset))
    $offset += $glyph.Length
    for ($i = 0; $i -lt $glyph.Length; $i++) {
        [void]$row.Add(('0x{0:X2}' -f $glyph[$i]))
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
[void]$sb.AppendLine('CONST UINTN gSfbFontGlyphCount = ' + $glyphs.Count + ';')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('CONST SFB_FONT_GLYPH gSfbFontGlyphs[] = {')
foreach ($gl in $glyphs) {
    [void]$sb.AppendLine('  ' + $gl + ',')
}
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')

[System.IO.File]::WriteAllText($OutFile, $sb.ToString(), (New-Object System.Text.ASCIIEncoding))
Write-Host ("Wrote {0} glyphs ({1} bytes bitmap) to {2}" -f $glyphs.Count, $offset, $OutFile)

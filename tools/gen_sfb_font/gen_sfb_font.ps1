# Generates SuperFbFontData.c from the UI strings in SuperFbLang.c.
#
# Renders every unique character used by the BDS UI with Noto Sans SC at 2x
# resolution, downsamples to a 32px-tall cell and stores 4-bit anti-aliased
# alpha (2 pixels per byte).  Metrics are unified so mixed-script text lines
# up cleanly:
#
#   - CJK ideographs and fullwidth forms (（），： etc.) all share one
#     full-width advance (26px, half the 52px render em) and the ink is
#     centred inside that cell, so Chinese text is perfectly even.
#   - Latin/ASCII stays proportional (ink width + 2px side padding), so
#     English words read naturally instead of being spaced like a grid.
#   - The full printable ASCII range is always included, so runtime
#     characters (PIN digits, '>' and '*' markers) can never come out as
#     placeholder boxes.
#
# The horizontal downsample really is 2:1: each final column averages a 2px
# render column, so glyphs keep their natural aspect ratio (a previous bug
# made the cell as wide as the ink and stretched every glyph 2x).
#
#   powershell -ExecutionPolicy Bypass -File gen_sfb_font.ps1
#
# The output file is written next to the input source; the C file is committed
# so the firmware build never needs to run this script.

$ErrorActionPreference = 'Stop'

$FontName     = 'Noto Sans SC'
$CellHeight   = 32
$Grid         = $CellHeight * 2          # 64x64 downsample source grid
$RenderSize   = 100                      # tall render canvas (no top clipping)
$FontPx       = 52
$AlphaLevels  = 16
$CellBaseline = 48                       # baseline row in the 64 grid (cell row 24)
$CjkAdvance   = 26                       # uniform full-width advance (CJK etc.)
$AdvancePad   = 2                        # side padding added to Latin ink widths
$SpaceAdvance = 8                        # advance for the space character

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$SrcFile = Join-Path $RepoRoot 'submodules\uefi\edk2\QcomModulePkg\Application\LinuxLoader\SuperFbLang.c'
$OutFile = Join-Path $RepoRoot 'submodules\uefi\edk2\QcomModulePkg\Application\LinuxLoader\SuperFbFontData.c'

if (!(Test-Path $SrcFile)) {
    Write-Error "Source strings file not found: $SrcFile"
}

Add-Type -AssemblyName System.Drawing

# ---- collect characters: ASCII range + everything used in L"..." literals --
$text = [System.IO.File]::ReadAllText($SrcFile, [System.Text.Encoding]::UTF8)
$matches = [regex]::Matches($text, 'L"((?:[^"\\]|\\.)*)"')
if ($matches.Count -eq 0) {
    Write-Error 'No L"..." string literals found in SuperFbLang.c'
}

$chars = New-Object 'System.Collections.Generic.HashSet[char]'
for ($code = 0x20; $code -le 0x7E; $code++) {
    [void]$chars.Add([char]$code)
}
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

# ---- character classification --------------------------------------------
# East Asian Wide / Fullwidth ranges: these get the uniform full-width cell.
function Test-WideChar([int]$Cp) {
    if ($Cp -ge 0x1100 -and $Cp -le 0x115F) { return $true }   # Hangul Jamo
    if ($Cp -ge 0x2E80 -and $Cp -le 0xA4CF) { return $true }   # CJK radicals,
                                                               # punctuation,
                                                               # ideographs,
                                                               # Hangul
    if ($Cp -ge 0xAC00 -and $Cp -le 0xD7A3) { return $true }   # Hangul syllables
    if ($Cp -ge 0xF900 -and $Cp -le 0xFAFF) { return $true }   # CJK compat
    if ($Cp -ge 0xFE10 -and $Cp -le 0xFE19) { return $true }   # vertical forms
    if ($Cp -ge 0xFE30 -and $Cp -le 0xFE6F) { return $true }   # CJK compat forms
    if ($Cp -ge 0xFF00 -and $Cp -le 0xFF60) { return $true }   # fullwidth forms
    if ($Cp -ge 0xFFE0 -and $Cp -le 0xFFE6) { return $true }   # fullwidth signs
    return $false
}

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

# ---- render one character into a packed 4-bit glyph -----------------------
function Get-Glyph([char]$Ch) {
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

    if ($maxX -lt 0) {
        # Blank glyph (e.g. space): a couple of empty pixels wide.
        $advance = if ([int]$Ch -eq 0x20) { $SpaceAdvance } else { 8 }
        return @{ Bytes = (New-Object byte[] (2 * $CellHeight / 2)); Width = 2; Advance = $advance }
    }

    $inkW = $maxX - $minX + 1
    $h = $maxY - $minY + 1

    # Wide characters (CJK, fullwidth punctuation) use one uniform full-width
    # cell with the ink centred; Latin keeps an ink-sized cell.
    $isWide = Test-WideChar ([int]$Ch)
    if ($isWide) {
        $cellW = $CjkAdvance
    } else {
        $cellW = [Math]::Ceiling($inkW / 2.0)
        if (($cellW % 2) -ne 0) { $cellW++ }   # packing needs an even width
    }

    # Centre the 2x sample window (cellW*2 render columns) over the ink.
    $off = [Math]::Max(0, [Math]::Floor(($cellW * 2 - $inkW) / 2))
    $pasteY = $CellBaseline - ($BaselineY - $minY)

    if ($pasteY -lt 0 -or $pasteY + $h -gt $Grid) {
        Write-Host ("WARNING: glyph U+{0:X4} does not fit the cell (y {1}..{2})" -f [int]$Ch, $pasteY, ($pasteY + $h - 1))
    }

    $out = New-Object byte[] (($cellW * $CellHeight) / 2)
    for ($y = 0; $y -lt $CellHeight; $y++) {
        for ($x = 0; $x -lt $cellW; $x++) {
            $sum = 0
            for ($jy = 0; $jy -lt 2; $jy++) {
                for ($jx = 0; $jx -lt 2; $jx++) {
                    # Horizontal: one final column averages a 2px render
                    # column, so the glyph keeps its natural aspect ratio.
                    # Vertical: the 32px cell is taller than most glyphs, so
                    # the pasteY offset is needed to anchor the baseline.
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

    $advance = if ($isWide) { $CjkAdvance } else { $cellW + $AdvancePad }
    return @{ Bytes = $out; Width = $cellW; Advance = $advance }
}

# ---- build the C source -----------------------------------------------------
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('/*')
[void]$sb.AppendLine(' * Generated by tools/gen_sfb_font/gen_sfb_font.ps1 - do not edit by hand.')
[void]$sb.AppendLine(" * Font: $FontName, ${CellHeight}px tall cells, 4-bit anti-aliased alpha.")
[void]$sb.AppendLine(" * CJK/fullwidth forms: ${CjkAdvance}px uniform advance; Latin: proportional advance.")
[void]$sb.AppendLine(' */')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#include "SuperFbFont.h"')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('CONST UINT8 gSfbFontBitmap[] = {')

$offset = 0
$glyphs = New-Object System.Collections.Generic.List[string]
$row = New-Object System.Collections.Generic.List[string]
foreach ($ch in $sorted) {
    $glyph = Get-Glyph $ch
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

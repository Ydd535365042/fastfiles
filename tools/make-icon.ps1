# Draws the FastFiles icon (a folder with a lightning bolt) at every Windows icon size and writes res/FastFiles.ico.
# Geometry is on a 256-unit grid; sizes <= 48 are stored as 32-bit DIBs, 64/256 as PNG. Run: powershell -File tools\make-icon.ps1
param([string]$Out = (Join-Path $PSScriptRoot '..\res\FastFiles.ico'), [string]$Preview = '')
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

function RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object Drawing.Drawing2D.GraphicsPath
    $p.AddArc($x, $y, 2 * $r, 2 * $r, 180, 90)
    $p.AddArc($x + $w - 2 * $r, $y, 2 * $r, 2 * $r, 270, 90)
    $p.AddArc($x + $w - 2 * $r, $y + $h - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $p.AddArc($x, $y + $h - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $p.CloseFigure()
    return $p
}

function Render([int]$size) {
    $bmp = New-Object Drawing.Bitmap($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([Drawing.Color]::Transparent)
    $k = $size / 256.0
    $small = $size -le 24
    $g.ScaleTransform($k, $k)
    # Small sizes: fill more of the square, fewer details
    $left = if ($small) { 12 } else { 22 }; $right = 256 - $left
    $tabTop = if ($small) { 30 } else { 40 }; $bodyTop = $tabTop + 20; $frontTop = if ($small) { 74 } else { 82 }
    $bottom = if ($small) { 232 } else { 218 }; $rad = if ($small) { 22 } else { 18 }

    if (-not $small) {
        # Soft shadow under the folder
        for ($i = 6; $i -ge 1; $i--) {
            $sp = RoundRect ($left - $i + 2) ($frontTop + 8 - $i) ($right - $left + 2 * $i - 4) ($bottom - $frontTop + 2 * $i - 4) ($rad + $i)
            $g.FillPath((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(7, 10, 30, 80))), $sp)
        }
    }

    # Back panel with the tab
    $back = New-Object Drawing.Drawing2D.GraphicsPath
    $tr = 14
    $back.AddArc($left, $tabTop, 2 * $tr, 2 * $tr, 180, 90)
    $back.AddLine(($left + $tr), $tabTop, 96, $tabTop)
    $back.AddBezier(96, $tabTop, 104, $tabTop, 108, $bodyTop, 118, $bodyTop)
    $back.AddArc($right - 2 * $rad, $bodyTop, 2 * $rad, 2 * $rad, 270, 90)
    $back.AddLine($right, ($bodyTop + $rad), $right, ($bottom - 20))
    $back.AddLine($left, ($bottom - 20), $left, ($tabTop + $tr))
    $back.CloseFigure()
    $backBrush = New-Object Drawing.Drawing2D.LinearGradientBrush((New-Object Drawing.PointF(0, $tabTop)), (New-Object Drawing.PointF(0, $bottom)),
        [Drawing.Color]::FromArgb(255, 0x1C, 0x4F, 0xC9), [Drawing.Color]::FromArgb(255, 0x12, 0x36, 0x9C))
    $g.FillPath($backBrush, $back)

    # Front panel
    $front = RoundRect $left $frontTop ($right - $left) ($bottom - $frontTop) $rad
    $frontBrush = New-Object Drawing.Drawing2D.LinearGradientBrush((New-Object Drawing.PointF(0, $frontTop)), (New-Object Drawing.PointF(0, $bottom)),
        [Drawing.Color]::FromArgb(255, 0x5C, 0xB4, 0xFF), [Drawing.Color]::FromArgb(255, 0x2A, 0x72, 0xEE))
    $g.FillPath($frontBrush, $front)
    if (-not $small) {
        # Light top edge
        $g.SetClip($front)
        $hl = New-Object Drawing.Pen ([Drawing.Color]::FromArgb(150, 0xC4, 0xE4, 0xFF)), 3
        $g.DrawLine($hl, $left, ($frontTop + 1.5), $right, ($frontTop + 1.5))
        $g.ResetClip()
    }

    # Lightning bolt, centered on the front panel
    $cy = ($frontTop + $bottom) / 2.0
    $s = if ($small) { 1.18 } else { 1.0 }
    $pts = @(@(16, -54), @(-26, 6), @(0, 6), @(-12, 54), @(32, -10), @(6, -10), @(24, -54)) | ForEach-Object {
        New-Object Drawing.PointF((128 + $_[0] * $s), ($cy + $_[1] * $s))
    }
    $bolt = New-Object Drawing.Drawing2D.GraphicsPath
    $bolt.AddPolygon([Drawing.PointF[]]$pts)
    if (-not $small) {
        $m = New-Object Drawing.Drawing2D.Matrix; $m.Translate(0, 4)
        $shadow = $bolt.Clone(); $shadow.Transform($m)
        $g.FillPath((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(70, 0x0A, 0x2E, 0x8A))), $shadow)
    }
    $g.FillPath([Drawing.Brushes]::White, $bolt)
    $g.Dispose()
    return $bmp
}

function PngBytes($bmp) { $ms = New-Object IO.MemoryStream; $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png); return $ms.ToArray() }

function DibBytes($bmp) {
    $n = $bmp.Width
    $ms = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter($ms)
    $w.Write([int]40); $w.Write([int]$n); $w.Write([int]($n * 2)); $w.Write([int16]1); $w.Write([int16]32)
    $w.Write([int]0); $w.Write([int]0); $w.Write([int]0); $w.Write([int]0); $w.Write([int]0); $w.Write([int]0)
    for ($y = $n - 1; $y -ge 0; $y--) { for ($x = 0; $x -lt $n; $x++) { $c = $bmp.GetPixel($x, $y); $w.Write([byte]$c.B); $w.Write([byte]$c.G); $w.Write([byte]$c.R); $w.Write([byte]$c.A) } }
    $stride = [int]([Math]::Ceiling($n / 32.0) * 4)
    for ($y = $n - 1; $y -ge 0; $y--) {
        $row = New-Object byte[] $stride
        for ($x = 0; $x -lt $n; $x++) { if ($bmp.GetPixel($x, $y).A -eq 0) { $row[[int][Math]::Floor($x / 8)] = $row[[int][Math]::Floor($x / 8)] -bor (0x80 -shr ($x % 8)) } }
        $w.Write($row)
    }
    $w.Flush(); return $ms.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = foreach ($sz in $sizes) { $b = Render $sz; [pscustomobject]@{ Size = $sz; Bmp = $b; Data = $(if ($sz -le 48) { DibBytes $b } else { PngBytes $b }) } }

$ms = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter($ms)
$w.Write([int16]0); $w.Write([int16]1); $w.Write([int16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($i in $images) {
    $d = if ($i.Size -ge 256) { 0 } else { $i.Size }
    $w.Write([byte]$d); $w.Write([byte]$d); $w.Write([byte]0); $w.Write([byte]0); $w.Write([int16]1); $w.Write([int16]32)
    $w.Write([int]$i.Data.Length); $w.Write([int]$offset); $offset += $i.Data.Length
}
foreach ($i in $images) { $w.Write([byte[]]$i.Data) }
$w.Flush()
[IO.File]::WriteAllBytes([IO.Path]::GetFullPath($Out), $ms.ToArray())
"wrote $Out ($($ms.Length) bytes)"

if ($Preview) {
    # All sizes side by side on light and dark backgrounds
    $sheet = New-Object Drawing.Bitmap(720, 600)
    $g = [Drawing.Graphics]::FromImage($sheet)
    $g.FillRectangle((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(0xF3, 0xF3, 0xF3))), 0, 0, 720, 300)
    $g.FillRectangle((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(0x20, 0x20, 0x20))), 0, 300, 720, 300)
    foreach ($row in 0, 300) {
        $x = 10
        foreach ($i in $images) { if ($i.Size -lt 256) { $g.DrawImage($i.Bmp, $x, $row + 20, $i.Size, $i.Size); $x += $i.Size + 14 } }
        $g.DrawImage($images[-1].Bmp, 440, $row + 22, 256, 256)
        # 4x magnified 16 and 32 to check pixels
        $g.InterpolationMode = 'NearestNeighbor'
        $g.DrawImage($images[0].Bmp, 10, $row + 110, 64, 64); $g.DrawImage($images[3].Bmp, 90, $row + 110, 128, 128)
    }
    $sheet.Save($Preview, [Drawing.Imaging.ImageFormat]::Png)
    "preview $Preview"
}

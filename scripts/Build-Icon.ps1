param([string]$OutputDirectory = (Join-Path $PSScriptRoot '..\app\assets'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
[void][IO.Directory]::CreateDirectory($OutputDirectory)

# Draw the N-shaped network path at a requested icon size, preserving transparent corners.
function New-LogoImage([int]$Size) {
    $bitmap = [Drawing.Bitmap]::new($Size, $Size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.ScaleTransform($Size / 256.0, $Size / 256.0)
    $background = [Drawing.Drawing2D.GraphicsPath]::new()
    $background.AddArc(8, 8, 112, 112, 180, 90)
    $background.AddArc(136, 8, 112, 112, 270, 90)
    $background.AddArc(136, 136, 112, 112, 0, 90)
    $background.AddArc(8, 136, 112, 112, 90, 90)
    $background.CloseFigure()
    $blue = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#275cc4'))
    $mint = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#55e5c0'))
    $pen = [Drawing.Pen]::new([Drawing.Color]::White, 24)
    $pen.StartCap = $pen.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
    try {
        $graphics.FillPath($blue, $background)
        $graphics.DrawLines($pen, [Drawing.PointF[]]@(
            [Drawing.PointF]::new(72, 180), [Drawing.PointF]::new(72, 76),
            [Drawing.PointF]::new(184, 180), [Drawing.PointF]::new(184, 76)))
        $graphics.FillEllipse($mint, 61, 169, 22, 22)
        $graphics.FillEllipse($mint, 173, 65, 22, 22)
    } finally {
        $graphics.Dispose(); $background.Dispose(); $blue.Dispose(); $mint.Dispose(); $pen.Dispose()
    }
    return $bitmap
}

$svg = @'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" role="img" aria-label="NSU 校园网">
  <rect x="8" y="8" width="240" height="240" rx="56" fill="#275cc4"/>
  <path d="M72 180V76L184 180V76" fill="none" stroke="white" stroke-width="24" stroke-linecap="round" stroke-linejoin="round"/>
  <g fill="#55e5c0"><circle cx="72" cy="180" r="11"/><circle cx="184" cy="76" r="11"/></g>
</svg>
'@
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'logo.svg'), $svg, [Text.UTF8Encoding]::new($false))
$sizes = @(16, 24, 32, 48, 64, 128, 256)
$frames = [Collections.Generic.List[byte[]]]::new()
foreach ($size in $sizes) {
    $bitmap = New-LogoImage $size
    $stream = [IO.MemoryStream]::new()
    try {
        $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
        $frames.Add($stream.ToArray())
        if ($size -eq 256) { $bitmap.Save((Join-Path $OutputDirectory 'logo.png'), [Drawing.Imaging.ImageFormat]::Png) }
    } finally { $bitmap.Dispose(); $stream.Dispose() }
}
$icon = [IO.File]::Create((Join-Path $OutputDirectory 'nsu.ico'))
$writer = [IO.BinaryWriter]::new($icon)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$frames[$i].Length); $writer.Write([uint32]$offset)
        $offset += $frames[$i].Length
    }
    foreach ($frame in $frames) { $writer.Write($frame) }
} finally { $writer.Dispose(); $icon.Dispose() }

# Draw a matching line icon for Wi-Fi signal waves or an Ethernet plug at native UI sizes.
function New-NetworkImage([int]$Size, [string]$Kind) {
    $bitmap = [Drawing.Bitmap]::new($Size, $Size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.ScaleTransform($Size / 256.0, $Size / 256.0)
    $color = [Drawing.ColorTranslator]::FromHtml('#275cc4')
    $pen = [Drawing.Pen]::new($color, 16)
    $pen.StartCap = $pen.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
    $brush = [Drawing.SolidBrush]::new($color)
    try {
        if ($Kind -eq 'wifi') {
            $graphics.DrawArc($pen, 16, 48, 224, 224, 225, 90)
            $graphics.DrawArc($pen, 56, 96, 144, 144, 225, 90)
            $graphics.DrawArc($pen, 96, 144, 64, 64, 225, 90)
            $graphics.FillEllipse($brush, 116, 188, 24, 24)
        } else {
            $graphics.DrawLines($pen, [Drawing.PointF[]]@(
                [Drawing.PointF]::new(64, 40), [Drawing.PointF]::new(192, 40),
                [Drawing.PointF]::new(192, 144), [Drawing.PointF]::new(160, 144),
                [Drawing.PointF]::new(160, 176), [Drawing.PointF]::new(96, 176),
                [Drawing.PointF]::new(96, 144), [Drawing.PointF]::new(64, 144),
                [Drawing.PointF]::new(64, 40)))
            foreach ($pin in @(88, 112, 136, 160)) { $graphics.DrawLine($pen, $pin, 64, $pin, 96) }
            $graphics.DrawLine($pen, 128, 176, 128, 224)
        }
    } finally { $graphics.Dispose(); $pen.Dispose(); $brush.Dispose() }
    return $bitmap
}

# Export reusable PNG previews and PNG-backed multi-size Windows icons for each connection type.
function Export-NetworkIcon([string]$Kind) {
    $iconSizes = @(16, 24, 32, 48, 64, 128, 256)
    $iconFrames = [Collections.Generic.List[byte[]]]::new()
    foreach ($size in $iconSizes) {
        $bitmap = New-NetworkImage $size $Kind
        $stream = [IO.MemoryStream]::new()
        try {
            $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
            $iconFrames.Add($stream.ToArray())
            if ($size -eq 256) { $bitmap.Save((Join-Path $OutputDirectory ($Kind + '.png')), [Drawing.Imaging.ImageFormat]::Png) }
        } finally { $bitmap.Dispose(); $stream.Dispose() }
    }
    $stream = [IO.File]::Create((Join-Path $OutputDirectory ($Kind + '.ico')))
    $writer = [IO.BinaryWriter]::new($stream)
    try {
        $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$iconSizes.Count)
        $offset = 6 + 16 * $iconSizes.Count
        for ($i = 0; $i -lt $iconSizes.Count; $i++) {
            $dimension = if ($iconSizes[$i] -eq 256) { 0 } else { $iconSizes[$i] }
            $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
            $writer.Write([byte]0); $writer.Write([byte]0)
            $writer.Write([uint16]1); $writer.Write([uint16]32)
            $writer.Write([uint32]$iconFrames[$i].Length); $writer.Write([uint32]$offset)
            $offset += $iconFrames[$i].Length
        }
        foreach ($frame in $iconFrames) { $writer.Write($frame) }
    } finally { $writer.Dispose(); $stream.Dispose() }
}

$wifiSvg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256"><g fill="none" stroke="#275cc4" stroke-width="16" stroke-linecap="round"><path d="M48.8 80.8A112 112 0 0 1 207.2 80.8M77.1 117.1A72 72 0 0 1 178.9 117.1M105.4 153.4A32 32 0 0 1 150.6 153.4"/></g><circle cx="128" cy="200" r="12" fill="#275cc4"/></svg>'
$ethernetSvg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256"><g fill="none" stroke="#275cc4" stroke-width="16" stroke-linecap="round" stroke-linejoin="round"><path d="M64 40H192V144H160V176H96V144H64ZM88 64V96M112 64V96M136 64V96M160 64V96M128 176V224"/></g></svg>'
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'wifi.svg'), $wifiSvg, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'ethernet.svg'), $ethernetSvg, [Text.UTF8Encoding]::new($false))
Export-NetworkIcon 'wifi'
Export-NetworkIcon 'ethernet'

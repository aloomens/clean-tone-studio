param([string]$OutPath, [int]$Size = 1024)
Add-Type -AssemblyName System.Drawing

$s = $Size / 512.0   # design on a 512 grid
$bmp = New-Object System.Drawing.Bitmap $Size, $Size
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
$g.Clear([System.Drawing.Color]::Transparent)

function RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}
function C([string]$hex, [int]$a = 255) {
    $c = [System.Drawing.ColorTranslator]::FromHtml($hex)
    return [System.Drawing.Color]::FromArgb($a, $c.R, $c.G, $c.B)
}

# tile
$tile = RoundRect (16*$s) (16*$s) (480*$s) (480*$s) (104*$s)
$bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(0, 16*$s)), ([System.Drawing.PointF]::new(0, 496*$s)), (C "#2e3036"), (C "#141518")
$g.FillPath($bgBrush, $tile)
$g.DrawPath((New-Object System.Drawing.Pen (C "#44474e"), (4*$s)), $tile)

# knob geometry: 270-degree travel starting bottom-left (GDI+ angles: 0 = 3 o'clock, clockwise)
$cx = 256*$s; $cy = 266*$s; $r = 160*$s
$arcRect = [System.Drawing.RectangleF]::new($cx - $r, $cy - $r, 2*$r, 2*$r)
$start = 135.0; $sweep = 270.0; $value = 0.72

$track = New-Object System.Drawing.Pen (C "#3c3f46"), (40*$s)
$track.StartCap = $track.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$g.DrawArc($track, $arcRect, $start, $sweep)

$amber = New-Object System.Drawing.Pen (C "#e8a54b"), (40*$s)
$amber.StartCap = $amber.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$g.DrawArc($amber, $arcRect, $start, $sweep * $value)

# knob body
$kr = 112*$s
$bodyRect = [System.Drawing.RectangleF]::new($cx - $kr, $cy - $kr, 2*$kr, 2*$kr)
$bodyBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush $bodyRect, (C "#4a4d55"), (C "#1c1d21"), 90.0
$g.FillEllipse($bodyBrush, $bodyRect)
$g.DrawEllipse((New-Object System.Drawing.Pen (C "#000000" 150), (4*$s)), $bodyRect)

# pointer, aimed at the end of the amber arc
$ang = ($start + $sweep * $value) * [Math]::PI / 180.0
$p1 = [System.Drawing.PointF]::new($cx + [Math]::Cos($ang) * 30*$s, $cy + [Math]::Sin($ang) * 30*$s)
$p2 = [System.Drawing.PointF]::new($cx + [Math]::Cos($ang) * 92*$s, $cy + [Math]::Sin($ang) * 92*$s)
$ptr = New-Object System.Drawing.Pen (C "#efe9df"), (26*$s)
$ptr.StartCap = $ptr.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$g.DrawLine($ptr, $p1, $p2)

$g.Dispose()
$bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"wrote $OutPath ($Size px)"

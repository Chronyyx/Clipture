# Keep the approved recorder render. Draw only the surrounding decoration in code.
# Installer lettering lives in native controls, not these bitmaps.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$artworkDirectory = Join-Path $projectRoot 'src-tauri/installer/artwork'
[IO.Directory]::CreateDirectory($artworkDirectory) | Out-Null
$logo = [Drawing.Image]::FromFile((Join-Path $projectRoot 'assets/clipture-logo.png'))
function Color([string]$Hex) { [Drawing.ColorTranslator]::FromHtml($Hex) }

function New-Artwork([string]$Name, [int]$Width, [int]$Height, [scriptblock]$Paint) {
    # 4x backing artwork stays detailed up to 400%; runtime fits actual pixels.
    $bitmap = [Drawing.Bitmap]::new($Width * 4, $Height * 4, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $g.ScaleTransform(4, 4)
        $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        & $Paint $g
        $bitmap.Save((Join-Path $artworkDirectory "$Name.bmp"), [Drawing.Imaging.ImageFormat]::Bmp)
    } finally { $g.Dispose(); $bitmap.Dispose() }
}

try {
    New-Artwork 'sidebar' 164 314 {
        param($g)
        $g.Clear((Color '#1B1C19'))
        $g.DrawImage($logo, [Drawing.RectangleF]::new(1, 79, 162, 162))
        $accent = [Drawing.SolidBrush]::new((Color '#FF6500'))
        $rule = [Drawing.Pen]::new((Color '#777D6E'), .8)
        try {
            $g.DrawLine($rule, 20, 274, 144, 274)
            for ($tick = 0; $tick -le 12; $tick++) {
                $x = 20 + $tick * (124 / 12)
                $height = if ($tick % 3 -eq 0) { 7 } else { 3 }
                $g.DrawLine($rule, $x, 274, $x, (274 - $height))
            }
            $g.FillRectangle($accent, 103, 263, 2, 15)
        } finally { $accent.Dispose(); $rule.Dispose() }
    }
    New-Artwork 'header' 150 57 {
        param($g)
        $g.Clear((Color '#F0F0EB'))
        $g.DrawImage($logo, [Drawing.RectangleF]::new(93, 0, 57, 57))
    }
} finally { $logo.Dispose() }
Write-Output "Original render preserved; 4x installer artwork written to $artworkDirectory"

param(
    [Parameter(Mandatory = $true)][string]$InputPath,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

# Convert an animated GIF (or a directory of same-sized numbered PNG frames)
# into a HORIZONTAL spritesheet PNG for the engine's SceneSprite sheet
# animation (equal-width sub-rects, one frame after another, left to right).
# Prints "<output>: <w>x<h>, <frames> frames" on success.
#
# Usage:
#   tools/gif_to_sheet.ps1 -InputPath walk.gif -OutputPath zombie.sheet.png
#   tools/gif_to_sheet.ps1 -InputPath frames_dir -OutputPath sun.sheet.png
#
# Requires Windows PowerShell 5.1 (System.Drawing). GIF frames are composited
# onto a transparent canvas at the GIF's logical screen size, so frames that
# only update a sub-rectangle keep the previous frame's pixels where the GIF
# disposal semantics ask for them (frame dedup: identical consecutive frames
# are dropped — animated GIFs often repeat frames to hold timing).

Add-Type -AssemblyName System.Drawing

$frames = New-Object System.Collections.Generic.List[System.Drawing.Bitmap]

if (Test-Path $InputPath -PathType Leaf) {
    $gif = [System.Drawing.Image]::FromFile($InputPath)
    $fd = New-Object System.Drawing.Imaging.FrameDimension $gif.FrameDimensionsList[0]
    $count = $gif.GetFrameCount($fd)
    $w = $gif.Width; $h = $gif.Height

    for ($i = 0; $i -lt $count; $i++) {
        $gif.SelectActiveFrame($fd, $i) | Out-Null
        $cur = New-Object System.Drawing.Bitmap $w, $h
        $g = [System.Drawing.Graphics]::FromImage($cur)
        $g.Clear([System.Drawing.Color]::Transparent)
        # After SelectActiveFrame the GIF decoder has already applied disposal
        # semantics internally, so DrawImage blits the fully-composited frame.
        $g.DrawImage($gif, [System.Drawing.Rectangle]::new(0, 0, $w, $h))
        $g.Dispose()
        $frames.Add($cur)
    }
    $gif.Dispose()
} elseif (Test-Path $InputPath -PathType Container) {
    $files = Get-ChildItem $InputPath -File -Filter *.png | Sort-Object {
        [int]($_.BaseName -replace '\D', '')
    }
    if ($files.Count -eq 0) { Write-Error "no PNG frames in '$InputPath'"; exit 1 }
    foreach ($f in $files) {
        $img = [System.Drawing.Image]::FromFile($f.FullName)
        $bmp = New-Object System.Drawing.Bitmap $img.Width, $img.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.DrawImage($img, 0, 0, $img.Width, $img.Height)
        $g.Dispose()
        $frames.Add($bmp)
        $img.Dispose()
    }
} else {
    Write-Error "input not found: '$InputPath'"
    exit 1
}

if ($frames.Count -eq 0) { Write-Error "no frames decoded from '$InputPath'"; exit 1 }

$fw = $frames[0].Width
$fh = $frames[0].Height
$sheet = New-Object System.Drawing.Bitmap ($fw * $frames.Count), $fh
$g = [System.Drawing.Graphics]::FromImage($sheet)
for ($i = 0; $i -lt $frames.Count; $i++) {
    $g.DrawImage($frames[$i], [System.Drawing.Rectangle]::new($i * $fw, 0, $fw, $fh))
}
$g.Dispose()
$sheet.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
$sheet.Dispose()
foreach ($f in $frames) { $f.Dispose() }

Write-Output ("{0}: {1}x{2}, {3} frames" -f $OutputPath, ($fw * $frames.Count), $fh, $frames.Count)

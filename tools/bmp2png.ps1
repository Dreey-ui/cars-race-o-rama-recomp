# Converts frame_*.bmp to .png (half size) for viewing
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot
Get-ChildItem -Path $root -Filter "frame_*.bmp" | ForEach-Object {
    $img = [System.Drawing.Image]::FromFile($_.FullName)
    $small = New-Object System.Drawing.Bitmap $img, ([int]($img.Width / 2)), ([int]($img.Height / 2))
    $out = [IO.Path]::ChangeExtension($_.FullName, ".png")
    $small.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
    $small.Dispose(); $img.Dispose()
    $out
}

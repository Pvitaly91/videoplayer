[CmdletBinding()]
param(
    [string]$SourcePng = (Join-Path $PSScriptRoot '..\src\VideoPlayer\assets\VideoPlayer.png'),
    [string]$DestinationIco = (Join-Path $PSScriptRoot '..\src\VideoPlayer\assets\VideoPlayer.ico')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$sourcePath = (Resolve-Path -LiteralPath $SourcePng).ProviderPath
$destinationPath = [IO.Path]::GetFullPath($DestinationIco)
if ([string]::Equals($sourcePath, $destinationPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The ICO destination must not replace the source PNG.'
}
if (-not (Test-Path -LiteralPath ([IO.Path]::GetDirectoryName($destinationPath)) -PathType Container)) {
    throw 'Create the destination asset directory first.'
}
$iconSizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$frames = [Collections.Generic.List[byte[]]]::new()
$sourceImage = [Drawing.Image]::FromFile($sourcePath)
try {
    if ($sourceImage.Width -ne $sourceImage.Height) { throw 'The app icon PNG must be square.' }
    foreach ($iconSize in $iconSizes) {
        $bitmap = [Drawing.Bitmap]::new($iconSize, $iconSize, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        $frameStream = [IO.MemoryStream]::new()
        try {
            $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
            $graphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
            $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $graphics.Clear([Drawing.Color]::Transparent)
            $graphics.DrawImage($sourceImage, [Drawing.Rectangle]::new(0, 0, $iconSize, $iconSize))
            if ($iconSize -eq 256) {
                # Windows Vista+ accepts a compressed PNG image in an ICO.
                $bitmap.Save($frameStream, [Drawing.Imaging.ImageFormat]::Png)
            } else {
                $writer = [IO.BinaryWriter]::new($frameStream, [Text.Encoding]::UTF8, $true)
                try {
                    $maskStride = [int]([Math]::Ceiling($iconSize / 32.0) * 4)
                    $writer.Write([uint32]40) # BITMAPINFOHEADER
                    $writer.Write([int32]$iconSize)
                    $writer.Write([int32]($iconSize * 2)) # XOR + AND height
                    $writer.Write([uint16]1)
                    $writer.Write([uint16]32)
                    $writer.Write([uint32]0) # BI_RGB
                    $writer.Write([uint32]($iconSize * $iconSize * 4))
                    foreach ($unused in 1..4) { $writer.Write([uint32]0) }
                    for ($y = $iconSize - 1; $y -ge 0; --$y) {
                        for ($x = 0; $x -lt $iconSize; ++$x) {
                            $pixel = $bitmap.GetPixel($x, $y)
                            $writer.Write([byte]$pixel.B)
                            $writer.Write([byte]$pixel.G)
                            $writer.Write([byte]$pixel.R)
                            $writer.Write([byte]$pixel.A)
                        }
                    }
                    for ($y = $iconSize - 1; $y -ge 0; --$y) {
                        $mask = [byte[]]::new($maskStride)
                        for ($x = 0; $x -lt $iconSize; ++$x) {
                            if ($bitmap.GetPixel($x, $y).A -eq 0) {
                                $byteIndex = [int][Math]::Floor($x / 8.0)
                                $mask[$byteIndex] = [byte]($mask[$byteIndex] -bor (0x80 -shr ($x % 8)))
                            }
                        }
                        $writer.Write($mask)
                    }
                    $writer.Flush()
                } finally { $writer.Dispose() }
            }
            $frames.Add($frameStream.ToArray())
        } finally {
            $frameStream.Dispose()
            $graphics.Dispose()
            $bitmap.Dispose()
        }
    }
} finally { $sourceImage.Dispose() }

$outputStream = [IO.MemoryStream]::new()
$outputWriter = [IO.BinaryWriter]::new($outputStream, [Text.Encoding]::UTF8, $true)
try {
    $outputWriter.Write([uint16]0)
    $outputWriter.Write([uint16]1) # ICO, not CUR
    $outputWriter.Write([uint16]$iconSizes.Count)
    $offset = 6 + 16 * $iconSizes.Count
    for ($index = 0; $index -lt $iconSizes.Count; ++$index) {
        $dimension = if ($iconSizes[$index] -eq 256) { 0 } else { $iconSizes[$index] }
        $outputWriter.Write([byte]$dimension)
        $outputWriter.Write([byte]$dimension)
        $outputWriter.Write([byte]0)
        $outputWriter.Write([byte]0)
        $outputWriter.Write([uint16]1)
        $outputWriter.Write([uint16]32)
        $outputWriter.Write([uint32]$frames[$index].Length)
        $outputWriter.Write([uint32]$offset)
        $offset += $frames[$index].Length
    }
    foreach ($frame in $frames) { $outputWriter.Write($frame) }
    $outputWriter.Flush()
    [IO.File]::WriteAllBytes($destinationPath, $outputStream.ToArray())
} finally {
    $outputWriter.Dispose()
    $outputStream.Dispose()
}
Write-Output "Created $destinationPath with $($iconSizes -join ', ') px frames."

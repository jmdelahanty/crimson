[CmdletBinding()]
param(
    [string]$DownloadRoot = "C:/third_party/downloads",
    [string]$ShareRoot = "\\YOUR-SERVER\crimson\windows-deps",
    [string]$ManifestVersion = $(Get-Date -Format "yyyy-MM-dd"),
    [string]$OpenCvArchive,
    [string]$TensorRtArchive,
    [string]$VideoCodecSdkArchive,
    [string]$FfmpegArchive,
    [switch]$CleanShare
)

$ErrorActionPreference = "Stop"

function Resolve-ArchivePath {
    param(
        [string]$ExplicitPath,
        [string]$SearchRoot,
        [string[]]$Patterns,
        [string]$Label
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (-not (Test-Path -LiteralPath $ExplicitPath)) {
            throw "$Label archive not found: $ExplicitPath"
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }

    if (-not (Test-Path -LiteralPath $SearchRoot)) {
        throw "Download root not found: $SearchRoot"
    }

    $matches = New-Object System.Collections.Generic.List[System.IO.FileInfo]
    foreach ($pattern in $Patterns) {
        Get-ChildItem -LiteralPath $SearchRoot -Filter $pattern -File -ErrorAction SilentlyContinue |
            ForEach-Object { [void]$matches.Add($_) }
    }

    $unique = $matches | Sort-Object FullName -Unique
    if ($unique.Count -eq 0) {
        throw "$Label archive not found in $SearchRoot"
    }
    if ($unique.Count -gt 1) {
        $names = $unique | ForEach-Object { $_.Name }
        throw "$Label archive is ambiguous in $SearchRoot: $($names -join ', ')"
    }
    return $unique[0].FullName
}

function Get-ArchiveMetadata {
    param(
        [string]$ArchivePath,
        [string]$DestinationName
    )

    $item = Get-Item -LiteralPath $ArchivePath
    $hash = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()

    return [ordered]@{
        filename = $item.Name
        sha256 = $hash
        destination = $DestinationName
    }
}

function Copy-ArchiveToShare {
    param(
        [string]$ArchivePath,
        [string]$TargetRoot
    )

    New-Item -ItemType Directory -Force -Path $TargetRoot | Out-Null
    Copy-Item -LiteralPath $ArchivePath -Destination (Join-Path $TargetRoot (Split-Path -Leaf $ArchivePath)) -Force
}

if ($CleanShare -and (Test-Path -LiteralPath $ShareRoot)) {
    Write-Host "Removing existing share contents under $ShareRoot"
    Get-ChildItem -LiteralPath $ShareRoot -Force | Remove-Item -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $ShareRoot | Out-Null

$resolvedOpenCvArchive = Resolve-ArchivePath -ExplicitPath $OpenCvArchive -SearchRoot $DownloadRoot -Patterns @("opencv*.zip", "opencv-install*.zip") -Label "OpenCV"
$resolvedTensorRtArchive = Resolve-ArchivePath -ExplicitPath $TensorRtArchive -SearchRoot $DownloadRoot -Patterns @("TensorRT-*.zip") -Label "TensorRT"
$resolvedVideoCodecSdkArchive = Resolve-ArchivePath -ExplicitPath $VideoCodecSdkArchive -SearchRoot $DownloadRoot -Patterns @("Video_Codec_SDK*.zip", "nvcodec*.zip") -Label "Video Codec SDK"
$resolvedFfmpegArchive = Resolve-ArchivePath -ExplicitPath $FfmpegArchive -SearchRoot $DownloadRoot -Patterns @("ffmpeg-nvidia*.zip", "ffmpeg*.zip") -Label "FFmpeg"

foreach ($archivePath in @(
    $resolvedOpenCvArchive,
    $resolvedTensorRtArchive,
    $resolvedVideoCodecSdkArchive,
    $resolvedFfmpegArchive
)) {
    Write-Host "Copying $(Split-Path -Leaf $archivePath) -> $ShareRoot"
    Copy-ArchiveToShare -ArchivePath $archivePath -TargetRoot $ShareRoot
}

$manifest = [ordered]@{
    version = $ManifestVersion
    archives = [ordered]@{
        OpenCV = (Get-ArchiveMetadata -ArchivePath $resolvedOpenCvArchive -DestinationName "opencv-install-4.10.0-x64")
        TensorRT = (Get-ArchiveMetadata -ArchivePath $resolvedTensorRtArchive -DestinationName "TensorRT-10.0.1.6")
        VideoCodecSdk = (Get-ArchiveMetadata -ArchivePath $resolvedVideoCodecSdkArchive -DestinationName "Video_Codec_SDK_13.0")
        FFmpeg = (Get-ArchiveMetadata -ArchivePath $resolvedFfmpegArchive -DestinationName "ffmpeg-nvidia")
    }
}

$manifestPath = Join-Path $ShareRoot "crimson-windows-deps.manifest.json"
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host ""
Write-Host "Published Crimson Windows dependency share:"
Write-Host "  $ShareRoot"
Write-Host ""
Write-Host "Wrote manifest:"
Write-Host "  $manifestPath"
Write-Host ""
Write-Host "Next step for Windows users:"
Write-Host "  . .\\tools\\setup_windows_from_internal_share.ps1 -ShareRoot `"$ShareRoot`" -CleanDestination -LoadDependencyRoots"

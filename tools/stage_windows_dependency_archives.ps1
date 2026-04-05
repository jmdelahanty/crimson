[CmdletBinding()]
param(
    [string]$DownloadRoot = "C:/third_party/downloads",
    [string]$DestinationRoot = "C:/third_party",
    [string]$OpenCvArchive,
    [string]$TensorRtArchive,
    [string]$VideoCodecSdkArchive,
    [string]$FfmpegArchive,
    [switch]$CleanDestination
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
        return $null
    }

    $matches = New-Object System.Collections.Generic.List[System.IO.FileInfo]
    foreach ($pattern in $Patterns) {
        Get-ChildItem -LiteralPath $SearchRoot -Filter $pattern -File -ErrorAction SilentlyContinue |
            ForEach-Object { [void]$matches.Add($_) }
    }

    $unique = $matches |
        Sort-Object FullName -Unique

    if ($unique.Count -eq 0) {
        return $null
    }

    if ($unique.Count -gt 1) {
        $names = $unique | ForEach-Object { $_.Name }
        throw "$Label archive is ambiguous in $SearchRoot: $($names -join ', ')"
    }

    return $unique[0].FullName
}

function Remove-IfRequested {
    param(
        [string]$TargetPath
    )

    if ($CleanDestination -and (Test-Path -LiteralPath $TargetPath)) {
        Write-Host "Removing existing destination: $TargetPath"
        Remove-Item -LiteralPath $TargetPath -Recurse -Force
    }
}

function Get-SingleExpandedRoot {
    param(
        [string]$TempExtractRoot
    )

    $entries = @(Get-ChildItem -LiteralPath $TempExtractRoot -Force)
    if ($entries.Count -eq 1 -and $entries[0].PSIsContainer) {
        return $entries[0].FullName
    }
    return $TempExtractRoot
}

function Expand-ArchiveToDestination {
    param(
        [string]$ArchivePath,
        [string]$DestinationPath,
        [string]$Label
    )

    if ([string]::IsNullOrWhiteSpace($ArchivePath)) {
        Write-Host "$Label archive not provided or not found. Skipping."
        return $false
    }

    Write-Host "Staging $Label from $ArchivePath"
    Remove-IfRequested -TargetPath $DestinationPath

    if (Test-Path -LiteralPath $DestinationPath) {
        throw "Destination already exists: $DestinationPath. Use -CleanDestination to replace it."
    }

    $tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("crimson-stage-" + [System.Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null

    try {
        Expand-Archive -LiteralPath $ArchivePath -DestinationPath $tempRoot -Force
        $expandedRoot = Get-SingleExpandedRoot -TempExtractRoot $tempRoot
        $parent = Split-Path -Parent $DestinationPath
        if (-not (Test-Path -LiteralPath $parent)) {
            New-Item -ItemType Directory -Force -Path $parent | Out-Null
        }
        Move-Item -LiteralPath $expandedRoot -Destination $DestinationPath
    } finally {
        if (Test-Path -LiteralPath $tempRoot) {
            Remove-Item -LiteralPath $tempRoot -Recurse -Force
        }
    }

    return $true
}

function Test-RequiredPath {
    param(
        [string]$PathValue,
        [string]$Label
    )

    if (-not (Test-Path -LiteralPath $PathValue)) {
        throw "$Label missing after staging: $PathValue"
    }
}

function Validate-OpenCv {
    param([string]$Root)
    Test-RequiredPath -PathValue $Root -Label "OpenCV root"
    $configCandidates = @(
        (Join-Path $Root "OpenCVConfig.cmake"),
        (Join-Path $Root "x64/vc17/lib/OpenCVConfig.cmake"),
        (Join-Path $Root "lib/OpenCVConfig.cmake")
    )
    if (-not ($configCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1)) {
        throw "OpenCVConfig.cmake not found under $Root"
    }
}

function Validate-TensorRt {
    param([string]$Root)
    Test-RequiredPath -PathValue (Join-Path $Root "include/NvInfer.h") -Label "TensorRT header NvInfer.h"
    Test-RequiredPath -PathValue (Join-Path $Root "include/NvInferVersion.h") -Label "TensorRT header NvInferVersion.h"
}

function Validate-VideoCodecSdk {
    param([string]$Root)
    Test-RequiredPath -PathValue (Join-Path $Root "Interface") -Label "Video Codec SDK Interface dir"
    $libCandidates = @(
        (Join-Path $Root "Lib/x64"),
        (Join-Path $Root "Lib/win/x64")
    )
    if (-not ($libCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1)) {
        throw "Video Codec SDK x64 lib directory not found under $Root"
    }
}

function Validate-Ffmpeg {
    param([string]$Root)
    Test-RequiredPath -PathValue (Join-Path $Root "include") -Label "FFmpeg include dir"
    Test-RequiredPath -PathValue (Join-Path $Root "lib") -Label "FFmpeg lib dir"
    Test-RequiredPath -PathValue (Join-Path $Root "bin/ffmpeg.exe") -Label "ffmpeg.exe"
    Test-RequiredPath -PathValue (Join-Path $Root "bin/ffprobe.exe") -Label "ffprobe.exe"
}

$resolvedOpenCvArchive = Resolve-ArchivePath -ExplicitPath $OpenCvArchive -SearchRoot $DownloadRoot -Patterns @("opencv*.zip", "opencv-install*.zip") -Label "OpenCV"
$resolvedTensorRtArchive = Resolve-ArchivePath -ExplicitPath $TensorRtArchive -SearchRoot $DownloadRoot -Patterns @("TensorRT-*.zip") -Label "TensorRT"
$resolvedVideoCodecSdkArchive = Resolve-ArchivePath -ExplicitPath $VideoCodecSdkArchive -SearchRoot $DownloadRoot -Patterns @("Video_Codec_SDK*.zip", "nvcodec*.zip") -Label "Video Codec SDK"
$resolvedFfmpegArchive = Resolve-ArchivePath -ExplicitPath $FfmpegArchive -SearchRoot $DownloadRoot -Patterns @("ffmpeg-nvidia*.zip", "ffmpeg*.zip") -Label "FFmpeg"

$openCvDest = Join-Path $DestinationRoot "opencv-install-4.10.0-x64"
$tensorRtDest = Join-Path $DestinationRoot "TensorRT-10.0.1.6"
$videoCodecSdkDest = Join-Path $DestinationRoot "Video_Codec_SDK_13.0"
$ffmpegDest = Join-Path $DestinationRoot "ffmpeg-nvidia"

$stagedAnything = $false
$stagedAnything = (Expand-ArchiveToDestination -ArchivePath $resolvedOpenCvArchive -DestinationPath $openCvDest -Label "OpenCV") -or $stagedAnything
$stagedAnything = (Expand-ArchiveToDestination -ArchivePath $resolvedTensorRtArchive -DestinationPath $tensorRtDest -Label "TensorRT") -or $stagedAnything
$stagedAnything = (Expand-ArchiveToDestination -ArchivePath $resolvedVideoCodecSdkArchive -DestinationPath $videoCodecSdkDest -Label "Video Codec SDK") -or $stagedAnything
$stagedAnything = (Expand-ArchiveToDestination -ArchivePath $resolvedFfmpegArchive -DestinationPath $ffmpegDest -Label "FFmpeg") -or $stagedAnything

if (-not $stagedAnything) {
    throw "No dependency archives were provided or auto-detected in $DownloadRoot."
}

if (Test-Path -LiteralPath $openCvDest) {
    Validate-OpenCv -Root $openCvDest
}
if (Test-Path -LiteralPath $tensorRtDest) {
    Validate-TensorRt -Root $tensorRtDest
}
if (Test-Path -LiteralPath $videoCodecSdkDest) {
    Validate-VideoCodecSdk -Root $videoCodecSdkDest
}
if (Test-Path -LiteralPath $ffmpegDest) {
    Validate-Ffmpeg -Root $ffmpegDest
}

Write-Host ""
Write-Host "Dependency archive staging complete."
Write-Host "Suggested next steps:"
Write-Host "  powershell -ExecutionPolicy Bypass -File .\\tools\\check_windows_prereqs.ps1"
Write-Host "  . .\\tools\\set_windows_dependency_roots.ps1"
Write-Host "  cmake --preset windows-trt10-cuda12.4-no-sfm"

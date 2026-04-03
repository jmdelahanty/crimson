[CmdletBinding()]
param(
    [string]$CudaToolkitRoot = "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.4",
    [string]$OpenCvDir = "C:/third_party/opencv-install-4.10.0-x64",
    [string]$FfmpegRoot = "C:/third_party/ffmpeg-nvidia",
    [string]$VideoCodecSdkRoot = "C:/third_party/Video_Codec_SDK_13.0",
    [string]$TensorRtRoot = "C:/third_party/TensorRT-10.0.1.6",
    [string]$VcpkgBinDir = "C:/src/vcpkg/installed/x64-windows/bin"
)

$ErrorActionPreference = "Stop"

function Show-PathStatus {
    param(
        [string]$Label,
        [string]$PathValue
    )

    $exists = Test-Path -LiteralPath $PathValue
    $status = if ($exists) { "OK" } else { "MISSING" }
    Write-Host ("{0,-32} {1,-8} {2}" -f $Label, $status, $PathValue)
}

function Resolve-OpenCvRuntimeDirs {
    param(
        [string]$PathValue
    )

    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        return @()
    }

    $candidates = @(
        (Join-Path $PathValue "x64\vc17\bin")
    )

    $leaf = Split-Path -Leaf $PathValue
    if ($leaf -ieq "lib") {
        $candidates += (Join-Path (Split-Path -Parent $PathValue) "bin")
    }

    return $candidates
}

function Prepend-ExistingPathEntries {
    param(
        [string[]]$Candidates
    )

    $existingEntries = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase
    )
    foreach ($entry in ($env:Path -split ';')) {
        if (-not [string]::IsNullOrWhiteSpace($entry)) {
            [void]$existingEntries.Add($entry)
        }
    }

    $entriesToAdd = New-Object System.Collections.Generic.List[string]
    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        if (-not (Test-Path -LiteralPath $candidate)) {
            continue
        }
        if ($existingEntries.Add($candidate)) {
            [void]$entriesToAdd.Add($candidate)
        }
    }

    if ($entriesToAdd.Count -gt 0) {
        $env:Path = (($entriesToAdd -join ';') + ';' + $env:Path)
    }

    return $entriesToAdd
}

$env:CRIMSON_CUDA_TOOLKIT_ROOT = $CudaToolkitRoot
$env:CRIMSON_OPENCV_DIR = $OpenCvDir
$env:CRIMSON_FFMPEG_ROOT = $FfmpegRoot
$env:CRIMSON_VIDEO_CODEC_SDK_ROOT = $VideoCodecSdkRoot
$env:CRIMSON_TENSORRT_ROOT = $TensorRtRoot

$runtimePathEntries = @(
    $VcpkgBinDir,
    (Join-Path $env:CRIMSON_CUDA_TOOLKIT_ROOT "bin"),
    (Join-Path $env:CRIMSON_FFMPEG_ROOT "bin"),
    (Join-Path $env:CRIMSON_TENSORRT_ROOT "bin"),
    (Join-Path $env:CRIMSON_TENSORRT_ROOT "lib")
)
$runtimePathEntries += Resolve-OpenCvRuntimeDirs -PathValue $env:CRIMSON_OPENCV_DIR
$addedRuntimePaths = Prepend-ExistingPathEntries -Candidates $runtimePathEntries

Write-Host "Crimson dependency roots loaded into the current PowerShell session."
Write-Host "Path status:"
Show-PathStatus -Label "CRIMSON_CUDA_TOOLKIT_ROOT" -PathValue $env:CRIMSON_CUDA_TOOLKIT_ROOT
Show-PathStatus -Label "CRIMSON_OPENCV_DIR" -PathValue $env:CRIMSON_OPENCV_DIR
Show-PathStatus -Label "CRIMSON_FFMPEG_ROOT" -PathValue $env:CRIMSON_FFMPEG_ROOT
Show-PathStatus -Label "CRIMSON_VIDEO_CODEC_SDK_ROOT" -PathValue $env:CRIMSON_VIDEO_CODEC_SDK_ROOT
Show-PathStatus -Label "CRIMSON_TENSORRT_ROOT" -PathValue $env:CRIMSON_TENSORRT_ROOT
Show-PathStatus -Label "VCPKG_BIN_DIR" -PathValue $VcpkgBinDir

Write-Host ""
Write-Host "Runtime PATH entries added for this session:"
if ($addedRuntimePaths.Count -eq 0) {
    Write-Host "  (no new entries were added)"
} else {
    foreach ($entry in $addedRuntimePaths) {
        Write-Host "  $entry"
    }
}

Write-Host ""
Write-Host "Next step:"
Write-Host "  cmake --preset windows-trt10-cuda12.4-no-sfm"

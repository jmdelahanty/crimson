[CmdletBinding()]
param(
    [string]$CudaToolkitRoot = "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.4",
    [string]$OpenCvDir = "C:/third_party/opencv-install-4.10.0",
    [string]$FfmpegRoot = "C:/third_party/ffmpeg-nvidia",
    [string]$VideoCodecSdkRoot = "C:/third_party/Video_Codec_SDK_13.0",
    [string]$TensorRtRoot = "C:/third_party/TensorRT-10.0.1.6"
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

$env:CRIMSON_CUDA_TOOLKIT_ROOT = $CudaToolkitRoot
$env:CRIMSON_OPENCV_DIR = $OpenCvDir
$env:CRIMSON_FFMPEG_ROOT = $FfmpegRoot
$env:CRIMSON_VIDEO_CODEC_SDK_ROOT = $VideoCodecSdkRoot
$env:CRIMSON_TENSORRT_ROOT = $TensorRtRoot

Write-Host "Crimson dependency roots loaded into the current PowerShell session."
Write-Host "Path status:"
Show-PathStatus -Label "CRIMSON_CUDA_TOOLKIT_ROOT" -PathValue $env:CRIMSON_CUDA_TOOLKIT_ROOT
Show-PathStatus -Label "CRIMSON_OPENCV_DIR" -PathValue $env:CRIMSON_OPENCV_DIR
Show-PathStatus -Label "CRIMSON_FFMPEG_ROOT" -PathValue $env:CRIMSON_FFMPEG_ROOT
Show-PathStatus -Label "CRIMSON_VIDEO_CODEC_SDK_ROOT" -PathValue $env:CRIMSON_VIDEO_CODEC_SDK_ROOT
Show-PathStatus -Label "CRIMSON_TENSORRT_ROOT" -PathValue $env:CRIMSON_TENSORRT_ROOT

Write-Host ""
Write-Host "Next step:"
Write-Host "  cmake --preset windows-trt10-cuda12.4-no-sfm"

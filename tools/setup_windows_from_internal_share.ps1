[CmdletBinding()]
param(
    [string]$ShareRoot = "\\YOUR-SERVER\crimson-windows-deps",
    [string]$DestinationRoot = "C:/third_party",
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [switch]$CleanDestination,
    [switch]$SkipPrereqCheck,
    [switch]$LoadDependencyRoots
)

$ErrorActionPreference = "Stop"

function Require-Path {
    param(
        [string]$PathValue,
        [string]$Label
    )

    if (-not (Test-Path -LiteralPath $PathValue)) {
        throw "$Label not found: $PathValue"
    }
}

$stageScript = Join-Path $PSScriptRoot "stage_windows_dependency_archives.ps1"
$checkScript = Join-Path $PSScriptRoot "check_windows_prereqs.ps1"
$setRootsScript = Join-Path $PSScriptRoot "set_windows_dependency_roots.ps1"

Require-Path -PathValue $ShareRoot -Label "Internal dependency share"
Require-Path -PathValue $stageScript -Label "stage_windows_dependency_archives.ps1"
Require-Path -PathValue $checkScript -Label "check_windows_prereqs.ps1"
Require-Path -PathValue $setRootsScript -Label "set_windows_dependency_roots.ps1"

$openCvDir = Join-Path $DestinationRoot "opencv-install-4.10.0-x64"
$tensorRtRoot = Join-Path $DestinationRoot "TensorRT-10.0.1.6"
$videoCodecSdkRoot = Join-Path $DestinationRoot "Video_Codec_SDK_13.0"
$ffmpegRoot = Join-Path $DestinationRoot "ffmpeg-nvidia"

Write-Host "Staging Crimson Windows dependencies from internal share:"
Write-Host "  $ShareRoot"

& $stageScript `
    -DownloadRoot $ShareRoot `
    -DestinationRoot $DestinationRoot `
    -RequireManifest `
    -CleanDestination:$CleanDestination

if (-not $SkipPrereqCheck) {
    Write-Host ""
    Write-Host "Running Windows prereq check..."
    & $checkScript `
        -RepoRoot $RepoRoot `
        -OpenCvDir $openCvDir `
        -TensorRtRoot $tensorRtRoot `
        -VideoCodecSdkRoot $videoCodecSdkRoot `
        -FfmpegRoot $ffmpegRoot
}

if ($LoadDependencyRoots) {
    Write-Host ""
    Write-Host "Loading Crimson dependency roots into the current PowerShell session..."
    . $setRootsScript `
        -OpenCvDir $openCvDir `
        -TensorRtRoot $tensorRtRoot `
        -VideoCodecSdkRoot $videoCodecSdkRoot `
        -FfmpegRoot $ffmpegRoot
} else {
    Write-Host ""
    Write-Host "If you want the dependency roots loaded into this same shell, rerun with:"
    Write-Host "  . .\tools\setup_windows_from_internal_share.ps1 -ShareRoot `"$ShareRoot`" -LoadDependencyRoots"
}

Write-Host ""
Write-Host "Suggested next steps:"
Write-Host "  cmake --preset windows-trt10-cuda12.4-no-sfm"
Write-Host "  cmake --build --preset build-windows-trt10-cuda12.4-no-sfm-release"

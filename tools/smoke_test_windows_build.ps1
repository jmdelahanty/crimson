[CmdletBinding()]
param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ConfigurePreset = "windows-trt10-cuda12.4-no-sfm",
    [string]$BuildPreset = "build-windows-trt10-cuda12.4-no-sfm-release",
    [string]$Configuration = "Release",
    [string]$InstallPrefix = "dist/Crimson",
    [switch]$RunInstall,
    [switch]$SkipPrereqCheck,
    [switch]$LoadDependencyRoots,
    [string]$CudaToolkitRoot = $(if ($env:CRIMSON_CUDA_TOOLKIT_ROOT) { $env:CRIMSON_CUDA_TOOLKIT_ROOT } else { "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.4" }),
    [string]$OpenCvDir = $(if ($env:CRIMSON_OPENCV_DIR) { $env:CRIMSON_OPENCV_DIR } else { "C:/third_party/opencv-install-4.10.0-x64" }),
    [string]$FfmpegRoot = $(if ($env:CRIMSON_FFMPEG_ROOT) { $env:CRIMSON_FFMPEG_ROOT } else { "C:/third_party/ffmpeg-nvidia" }),
    [string]$VideoCodecSdkRoot = $(if ($env:CRIMSON_VIDEO_CODEC_SDK_ROOT) { $env:CRIMSON_VIDEO_CODEC_SDK_ROOT } else { "C:/third_party/Video_Codec_SDK_13.0" }),
    [string]$TensorRtRoot = $(if ($env:CRIMSON_TENSORRT_ROOT) { $env:CRIMSON_TENSORRT_ROOT } else { "C:/third_party/TensorRT-10.0.1.6" }),
    [string]$VcpkgBinDir = "C:/src/vcpkg/installed/x64-windows/bin"
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

function Invoke-Step {
    param(
        [string]$Label,
        [scriptblock]$Action
    )

    Write-Host ""
    Write-Host "== $Label =="
    & $Action
}

function Invoke-NativeAndRequireSuccess {
    param(
        [string]$Label,
        [scriptblock]$Action
    )

    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE"
    }
}

function Require-ExistingCandidate {
    param(
        [string]$Label,
        [string[]]$Candidates
    )

    foreach ($candidate in $Candidates) {
        if (Test-Path -LiteralPath $candidate) {
            Write-Host "$Label: $candidate"
            return $candidate
        }
    }

    throw "$Label not found. Checked: $($Candidates -join ', ')"
}

$setRootsScript = Join-Path $PSScriptRoot "set_windows_dependency_roots.ps1"
$checkPrereqsScript = Join-Path $PSScriptRoot "check_windows_prereqs.ps1"

Require-Path -PathValue $RepoRoot -Label "Repo root"
Require-Path -PathValue $setRootsScript -Label "set_windows_dependency_roots.ps1"
Require-Path -PathValue $checkPrereqsScript -Label "check_windows_prereqs.ps1"

if ($LoadDependencyRoots) {
    Invoke-Step -Label "Load Dependency Roots" -Action {
        . $setRootsScript `
            -CudaToolkitRoot $CudaToolkitRoot `
            -OpenCvDir $OpenCvDir `
            -FfmpegRoot $FfmpegRoot `
            -VideoCodecSdkRoot $VideoCodecSdkRoot `
            -TensorRtRoot $TensorRtRoot `
            -VcpkgBinDir $VcpkgBinDir
    }
}

if (-not $SkipPrereqCheck) {
    Invoke-Step -Label "Prereq Check" -Action {
        Invoke-NativeAndRequireSuccess -Label "Prereq check" -Action {
            & $checkPrereqsScript `
                -RepoRoot $RepoRoot `
                -CudaToolkitRoot $CudaToolkitRoot `
                -OpenCvDir $OpenCvDir `
                -FfmpegRoot $FfmpegRoot `
                -VideoCodecSdkRoot $VideoCodecSdkRoot `
                -TensorRtRoot $TensorRtRoot `
                -VcpkgBinDir $VcpkgBinDir
        }
    }
}

Invoke-Step -Label "Configure" -Action {
    Push-Location $RepoRoot
    try {
        Invoke-NativeAndRequireSuccess -Label "Configure" -Action {
            & cmake --preset $ConfigurePreset
        }
    } finally {
        Pop-Location
    }
}

Invoke-Step -Label "Build" -Action {
    Push-Location $RepoRoot
    try {
        Invoke-NativeAndRequireSuccess -Label "Build" -Action {
            & cmake --build --preset $BuildPreset
        }
    } finally {
        Pop-Location
    }
}

$builtExeCandidates = @(
    (Join-Path $RepoRoot "release/Release/redgui.exe"),
    (Join-Path $RepoRoot "release/redgui.exe")
)

$builtExe = Require-ExistingCandidate -Label "Built redgui.exe" -Candidates $builtExeCandidates

$stagedExe = $null
if ($RunInstall) {
    $binaryDir = Join-Path $RepoRoot ("build/" + $ConfigurePreset)
    Invoke-Step -Label "Install" -Action {
        Push-Location $RepoRoot
        try {
            Invoke-NativeAndRequireSuccess -Label "Install" -Action {
                & cmake --install $binaryDir --config $Configuration --prefix $InstallPrefix
            }
        } finally {
            Pop-Location
        }
    }

    $stagedExeCandidates = @(
        (Join-Path $RepoRoot (Join-Path $InstallPrefix "bin/redgui.exe")),
        (Join-Path $RepoRoot (Join-Path $InstallPrefix "redgui.exe"))
    )
    $stagedExe = Require-ExistingCandidate -Label "Installed redgui.exe" -Candidates $stagedExeCandidates
}

Write-Host ""
Write-Host "Smoke test completed successfully."
Write-Host "Summary:"
Write-Host "  Configure preset: $ConfigurePreset"
Write-Host "  Build preset:     $BuildPreset"
Write-Host "  Built exe:        $builtExe"
if ($RunInstall -and $stagedExe) {
    Write-Host "  Installed exe:    $stagedExe"
}
Write-Host ""
Write-Host "Suggested next step:"
Write-Host "  Launch redgui and open a representative recording."

[CmdletBinding()]
param(
    [string]$Preset = "windows-trt10-cuda12.4-no-sfm",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$InstallPrefix = "dist/Crimson",
    [string]$ThirdPartyRoot = "C:/third_party",
    [string]$CudaToolkitRoot = "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.4",
    [string]$OpenCvRoot,
    [string]$OpenCvDir,
    [string]$FfmpegRoot,
    [string]$VideoCodecSdkRoot,
    [string]$TensorRtRoot,
    [string]$VcpkgBinDir = "C:/src/vcpkg/installed/x64-windows/bin",
    [switch]$SkipDependencySetup,
    [switch]$SkipSubmodules,
    [switch]$SkipConfigure,
    [switch]$SkipBuild,
    [switch]$SkipInstall,
    [switch]$SkipRuntimeCheck,
    [switch]$RequireNvidiaSmi,
    [switch]$CleanInstall,
    [switch]$Launch
)

$ErrorActionPreference = "Stop"

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

function Resolve-FirstExistingPath {
    param(
        [string[]]$Candidates,
        [string]$Fallback
    )

    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        if (Test-Path -LiteralPath $candidate) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }

    return $Fallback
}

function Resolve-OpenCvConfigDir {
    param(
        [string]$ExplicitConfigDir,
        [string]$Root
    )

    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($ExplicitConfigDir)) {
        $candidates += $ExplicitConfigDir
    }
    if (-not [string]::IsNullOrWhiteSpace($Root)) {
        $candidates += @(
            (Join-Path $Root "lib/cmake/opencv4"),
            (Join-Path $Root "x64/vc17/lib"),
            (Join-Path $Root "build"),
            $Root
        )
    }

    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        if (Test-Path -LiteralPath (Join-Path $candidate "OpenCVConfig.cmake")) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($ExplicitConfigDir)) {
        return $ExplicitConfigDir
    }
    return $Root
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

function Require-Command {
    param([string]$Name)

    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command not found in PATH: $Name"
    }
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable,
        [string[]]$Arguments = @()
    )

    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Executable $($Arguments -join ' ')"
    }
}

function Require-ExistingPath {
    param(
        [string]$Label,
        [string]$PathValue
    )

    if (-not (Test-Path -LiteralPath $PathValue)) {
        throw "$Label not found: $PathValue"
    }
}

if ([string]::IsNullOrWhiteSpace($OpenCvRoot)) {
    $OpenCvRoot = Join-Path $ThirdPartyRoot "opencv-install-4.10.0-x64"
}
if ([string]::IsNullOrWhiteSpace($FfmpegRoot)) {
    $FfmpegRoot = Join-Path $ThirdPartyRoot "ffmpeg-nvidia"
}
if ([string]::IsNullOrWhiteSpace($TensorRtRoot)) {
    $TensorRtRoot = Join-Path $ThirdPartyRoot "TensorRT-10.0.1.6"
}
if ([string]::IsNullOrWhiteSpace($VideoCodecSdkRoot)) {
    $VideoCodecSdkRoot = Resolve-FirstExistingPath `
        -Candidates @(
            (Join-Path $ThirdPartyRoot "Video_Codec_SDK_13.0.19"),
            (Join-Path $ThirdPartyRoot "Video_Codec_SDK_13.0")
        ) `
        -Fallback (Join-Path $ThirdPartyRoot "Video_Codec_SDK_13.0.19")
}

$OpenCvDir = Resolve-OpenCvConfigDir -ExplicitConfigDir $OpenCvDir -Root $OpenCvRoot
$InstallPrefixPath = if ([System.IO.Path]::IsPathRooted($InstallPrefix)) {
    [System.IO.Path]::GetFullPath($InstallPrefix)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $InstallPrefix))
}
$BuildDir = Join-Path $RepoRoot ("build/" + $Preset)

Write-Host "Crimson Windows app-drop build"
Write-Host "  repo:          $RepoRoot"
Write-Host "  preset:        $Preset"
Write-Host "  config:        $Configuration"
Write-Host "  build dir:     $BuildDir"
Write-Host "  install root:  $InstallPrefixPath"

Invoke-Step "Tool check" {
    Require-Command cmake
    Require-Command git
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
        Write-Host "ninja was not found in PATH. This is OK only if CMake can still find Ninja from the active Visual Studio developer shell."
    }
    Invoke-NativeCommand -Executable cmake -Arguments @("--version")
}

if (-not $SkipSubmodules) {
    Invoke-Step "Submodules" {
        Invoke-NativeCommand -Executable git -Arguments @("-C", $RepoRoot, "submodule", "update", "--init", "--recursive")
    }
}

if (-not $SkipDependencySetup) {
    Invoke-Step "Dependency roots" {
        $dependencyScript = Join-Path $RepoRoot "tools/set_windows_dependency_roots.ps1"
        if (-not (Test-Path -LiteralPath $dependencyScript)) {
            throw "Dependency root helper not found: $dependencyScript"
        }

        . $dependencyScript `
            -CudaToolkitRoot $CudaToolkitRoot `
            -OpenCvDir $OpenCvDir `
            -FfmpegRoot $FfmpegRoot `
            -VideoCodecSdkRoot $VideoCodecSdkRoot `
            -TensorRtRoot $TensorRtRoot `
            -VcpkgBinDir $VcpkgBinDir
    }
}

if (-not $SkipConfigure) {
    Invoke-Step "Required dependency roots" {
        Require-ExistingPath -Label "CUDA Toolkit root" -PathValue $CudaToolkitRoot
        Require-ExistingPath -Label "CUDA nvcc" -PathValue (Join-Path $CudaToolkitRoot "bin/nvcc.exe")
        Require-ExistingPath -Label "TensorRT root" -PathValue $TensorRtRoot
    }
}

if (-not $SkipConfigure) {
    Invoke-Step "Configure" {
        Invoke-NativeCommand -Executable cmake -Arguments @("--preset", $Preset)
    }
}

if (-not $SkipBuild) {
    Invoke-Step "Build" {
        Invoke-NativeCommand -Executable cmake -Arguments @("--build", $BuildDir, "--config", $Configuration)
    }
}

if (-not $SkipInstall) {
    Invoke-Step "Install app drop" {
        if ($CleanInstall -and (Test-Path -LiteralPath $InstallPrefixPath)) {
            Remove-Item -LiteralPath $InstallPrefixPath -Recurse -Force
        }
        Invoke-NativeCommand -Executable cmake -Arguments @("--install", $BuildDir, "--config", $Configuration, "--prefix", $InstallPrefixPath)
    }
}

if (-not $SkipRuntimeCheck) {
    Invoke-Step "Runtime check" {
        $runtimeCheckScript = Join-Path $InstallPrefixPath "check_crimson_runtime.ps1"
        if (-not (Test-Path -LiteralPath $runtimeCheckScript)) {
            throw "Runtime check script not found after install: $runtimeCheckScript"
        }
        if ($RequireNvidiaSmi) {
            & $runtimeCheckScript -AppRoot $InstallPrefixPath -RequireNvidiaSmi
        } else {
            & $runtimeCheckScript -AppRoot $InstallPrefixPath
        }
    }
}

$exePath = Join-Path $InstallPrefixPath "bin/redgui.exe"
Write-Host ""
Write-Host "Done."
Write-Host "  app: $exePath"

if ($Launch) {
    Invoke-Step "Launch" {
        if (-not (Test-Path -LiteralPath $exePath)) {
            throw "Executable not found: $exePath"
        }
        & $exePath
    }
} else {
    Write-Host ""
    Write-Host "Launch command:"
    Write-Host "  & `"$exePath`""
}

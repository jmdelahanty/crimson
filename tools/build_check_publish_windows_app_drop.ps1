[CmdletBinding()]
param(
    [string]$Preset = "windows-trt10-cuda12.4-no-sfm",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$InstallPrefix = "dist/Crimson",
    [string]$BuildDir,
    [string]$ThirdPartyRoot = "C:/third_party",
    [string]$CudaToolkitRoot = "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.4",
    [string]$OpenCvRoot,
    [string]$OpenCvDir,
    [string]$FfmpegRoot,
    [string]$VideoCodecSdkRoot,
    [string]$TensorRtRoot,
    [string]$VcpkgRoot = "C:/src/vcpkg",
    [string]$VcpkgTriplet = "x64-windows",
    [string]$VcpkgBinDir,
    [string]$Python3Executable,
    [string]$NasmExecutable,
    [switch]$SkipDependencySetup,
    [switch]$SkipSubmodules,
    [switch]$SkipConfigure,
    [switch]$SkipBuild,
    [switch]$SkipInstall,
    [switch]$RequireNvidiaSmi,
    [switch]$CleanInstall,

    [string]$ShareRoot,
    [string]$ReleaseName,
    [switch]$PublishCurrent,
    [switch]$ArchiveExistingCurrent,
    [string]$CurrentName = "current",
    [string]$ReleasesDirName = "releases"
)

$ErrorActionPreference = "Stop"

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$BuildScript = Join-Path $PSScriptRoot "build_windows_app_drop.ps1"
$RuntimeCheckScript = Join-Path $PSScriptRoot "check_crimson_runtime.ps1"
$PublishScript = Join-Path $PSScriptRoot "publish_windows_app_drop.ps1"

function Resolve-FullPath {
    param([string]$PathValue)

    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        return $PathValue
    }
    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return [System.IO.Path]::GetFullPath($PathValue)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $PathValue))
}

function Add-OptionalArgument {
    param(
        [System.Collections.ArrayList]$Arguments,
        [string]$Name,
        [string]$Value
    )

    if (-not [string]::IsNullOrWhiteSpace($Value)) {
        [void]$Arguments.Add($Name)
        [void]$Arguments.Add($Value)
    }
}

function Add-SwitchArgument {
    param(
        [System.Collections.ArrayList]$Arguments,
        [string]$Name,
        [bool]$Enabled
    )

    if ($Enabled) {
        [void]$Arguments.Add($Name)
    }
}

function Invoke-Script {
    param(
        [string]$Label,
        [string]$ScriptPath,
        [string[]]$Arguments
    )

    if (-not (Test-Path -LiteralPath $ScriptPath)) {
        throw "$Label script not found: $ScriptPath"
    }

    Write-Host ""
    Write-Host "== $Label =="
    & $ScriptPath @Arguments
}

if ([string]::IsNullOrWhiteSpace($ShareRoot)) {
    throw "Pass -ShareRoot with the Windows-visible publish share, for example: -ShareRoot `"\\SERVER\crimson\windows-app`""
}
if (-not $PublishCurrent -and [string]::IsNullOrWhiteSpace($ReleaseName)) {
    throw "Pass -PublishCurrent to publish and refresh the current app drop, or pass -ReleaseName for a versioned-only publish."
}
if ($ArchiveExistingCurrent -and -not $PublishCurrent) {
    throw "-ArchiveExistingCurrent requires -PublishCurrent."
}

$InstallRoot = Resolve-FullPath -PathValue $InstallPrefix
$buildArgs = [System.Collections.ArrayList]::new()
[void]$buildArgs.AddRange(@(
    "-Preset", $Preset,
    "-Configuration", $Configuration,
    "-InstallPrefix", $InstallPrefix,
    "-ThirdPartyRoot", $ThirdPartyRoot,
    "-CudaToolkitRoot", $CudaToolkitRoot,
    "-VcpkgRoot", $VcpkgRoot,
    "-VcpkgTriplet", $VcpkgTriplet,
    "-SkipRuntimeCheck"
))
Add-OptionalArgument -Arguments $buildArgs -Name "-BuildDir" -Value $BuildDir
Add-OptionalArgument -Arguments $buildArgs -Name "-OpenCvRoot" -Value $OpenCvRoot
Add-OptionalArgument -Arguments $buildArgs -Name "-OpenCvDir" -Value $OpenCvDir
Add-OptionalArgument -Arguments $buildArgs -Name "-FfmpegRoot" -Value $FfmpegRoot
Add-OptionalArgument -Arguments $buildArgs -Name "-VideoCodecSdkRoot" -Value $VideoCodecSdkRoot
Add-OptionalArgument -Arguments $buildArgs -Name "-TensorRtRoot" -Value $TensorRtRoot
Add-OptionalArgument -Arguments $buildArgs -Name "-VcpkgBinDir" -Value $VcpkgBinDir
Add-OptionalArgument -Arguments $buildArgs -Name "-Python3Executable" -Value $Python3Executable
Add-OptionalArgument -Arguments $buildArgs -Name "-NasmExecutable" -Value $NasmExecutable
Add-SwitchArgument -Arguments $buildArgs -Name "-SkipDependencySetup" -Enabled $SkipDependencySetup
Add-SwitchArgument -Arguments $buildArgs -Name "-SkipSubmodules" -Enabled $SkipSubmodules
Add-SwitchArgument -Arguments $buildArgs -Name "-SkipConfigure" -Enabled $SkipConfigure
Add-SwitchArgument -Arguments $buildArgs -Name "-SkipBuild" -Enabled $SkipBuild
Add-SwitchArgument -Arguments $buildArgs -Name "-SkipInstall" -Enabled $SkipInstall
Add-SwitchArgument -Arguments $buildArgs -Name "-CleanInstall" -Enabled $CleanInstall

$checkArgs = [System.Collections.ArrayList]::new()
[void]$checkArgs.AddRange(@("-AppRoot", $InstallRoot))
Add-SwitchArgument -Arguments $checkArgs -Name "-RequireNvidiaSmi" -Enabled $RequireNvidiaSmi

$publishArgs = [System.Collections.ArrayList]::new()
[void]$publishArgs.AddRange(@(
    "-StageRoot", $InstallRoot,
    "-ShareRoot", $ShareRoot,
    "-CurrentName", $CurrentName,
    "-ReleasesDirName", $ReleasesDirName
))
Add-OptionalArgument -Arguments $publishArgs -Name "-ReleaseName" -Value $ReleaseName
Add-SwitchArgument -Arguments $publishArgs -Name "-PublishCurrent" -Enabled $PublishCurrent
Add-SwitchArgument -Arguments $publishArgs -Name "-ArchiveExistingCurrent" -Enabled $ArchiveExistingCurrent

Write-Host "Crimson Windows build/check/publish"
Write-Host "  repo:         $RepoRoot"
Write-Host "  preset:       $Preset"
Write-Host "  config:       $Configuration"
Write-Host "  install root: $InstallRoot"
Write-Host "  share root:   $ShareRoot"
if ([string]::IsNullOrWhiteSpace($ReleaseName)) {
    Write-Host "  release name: auto"
} else {
    Write-Host "  release name: $ReleaseName"
}
Write-Host "  publish current: $($PublishCurrent.IsPresent)"
Write-Host "  archive current: $($ArchiveExistingCurrent.IsPresent)"

Invoke-Script -Label "Build and stage app drop" `
    -ScriptPath $BuildScript `
    -Arguments $buildArgs.ToArray()

Invoke-Script -Label "Runtime check" `
    -ScriptPath $RuntimeCheckScript `
    -Arguments $checkArgs.ToArray()

Invoke-Script -Label "Publish app drop" `
    -ScriptPath $PublishScript `
    -Arguments $publishArgs.ToArray()

Write-Host ""
Write-Host "Build/check/publish complete."

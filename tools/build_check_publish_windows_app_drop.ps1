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

function Add-OptionalParameter {
    param(
        [hashtable]$Parameters,
        [string]$Name,
        [string]$Value
    )

    if (-not [string]::IsNullOrWhiteSpace($Value)) {
        $Parameters[$Name] = $Value
    }
}

function Add-SwitchParameter {
    param(
        [hashtable]$Parameters,
        [string]$Name,
        [bool]$Enabled
    )

    if ($Enabled) {
        $Parameters[$Name] = $true
    }
}

function Invoke-Script {
    param(
        [string]$Label,
        [string]$ScriptPath,
        [hashtable]$Parameters
    )

    if (-not (Test-Path -LiteralPath $ScriptPath)) {
        throw "$Label script not found: $ScriptPath"
    }

    Write-Host ""
    Write-Host "== $Label =="
    & $ScriptPath @Parameters
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
$buildParams = @{
    Preset = $Preset
    Configuration = $Configuration
    InstallPrefix = $InstallPrefix
    ThirdPartyRoot = $ThirdPartyRoot
    CudaToolkitRoot = $CudaToolkitRoot
    VcpkgRoot = $VcpkgRoot
    VcpkgTriplet = $VcpkgTriplet
    SkipRuntimeCheck = $true
}
Add-OptionalParameter -Parameters $buildParams -Name "BuildDir" -Value $BuildDir
Add-OptionalParameter -Parameters $buildParams -Name "OpenCvRoot" -Value $OpenCvRoot
Add-OptionalParameter -Parameters $buildParams -Name "OpenCvDir" -Value $OpenCvDir
Add-OptionalParameter -Parameters $buildParams -Name "FfmpegRoot" -Value $FfmpegRoot
Add-OptionalParameter -Parameters $buildParams -Name "VideoCodecSdkRoot" -Value $VideoCodecSdkRoot
Add-OptionalParameter -Parameters $buildParams -Name "TensorRtRoot" -Value $TensorRtRoot
Add-OptionalParameter -Parameters $buildParams -Name "VcpkgBinDir" -Value $VcpkgBinDir
Add-OptionalParameter -Parameters $buildParams -Name "Python3Executable" -Value $Python3Executable
Add-OptionalParameter -Parameters $buildParams -Name "NasmExecutable" -Value $NasmExecutable
Add-SwitchParameter -Parameters $buildParams -Name "SkipDependencySetup" -Enabled $SkipDependencySetup.IsPresent
Add-SwitchParameter -Parameters $buildParams -Name "SkipSubmodules" -Enabled $SkipSubmodules.IsPresent
Add-SwitchParameter -Parameters $buildParams -Name "SkipConfigure" -Enabled $SkipConfigure.IsPresent
Add-SwitchParameter -Parameters $buildParams -Name "SkipBuild" -Enabled $SkipBuild.IsPresent
Add-SwitchParameter -Parameters $buildParams -Name "SkipInstall" -Enabled $SkipInstall.IsPresent
Add-SwitchParameter -Parameters $buildParams -Name "CleanInstall" -Enabled $CleanInstall.IsPresent

$checkParams = @{
    AppRoot = $InstallRoot
}
Add-SwitchParameter -Parameters $checkParams -Name "RequireNvidiaSmi" -Enabled $RequireNvidiaSmi.IsPresent

$publishParams = @{
    StageRoot = $InstallRoot
    ShareRoot = $ShareRoot
    CurrentName = $CurrentName
    ReleasesDirName = $ReleasesDirName
}
Add-OptionalParameter -Parameters $publishParams -Name "ReleaseName" -Value $ReleaseName
Add-SwitchParameter -Parameters $publishParams -Name "PublishCurrent" -Enabled $PublishCurrent.IsPresent
Add-SwitchParameter -Parameters $publishParams -Name "ArchiveExistingCurrent" -Enabled $ArchiveExistingCurrent.IsPresent

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
    -Parameters $buildParams

Invoke-Script -Label "Runtime check" `
    -ScriptPath $RuntimeCheckScript `
    -Parameters $checkParams

Invoke-Script -Label "Publish app drop" `
    -ScriptPath $PublishScript `
    -Parameters $publishParams

Write-Host ""
Write-Host "Build/check/publish complete."

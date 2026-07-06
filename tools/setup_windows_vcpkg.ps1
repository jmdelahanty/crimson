[CmdletBinding()]
param(
    [string]$VcpkgRoot = "C:/src/vcpkg",
    [string]$Triplet = "x64-windows",
    [string]$VcpkgRepository = "https://github.com/microsoft/vcpkg.git",
    [string[]]$Packages = @("glew", "glfw3", "hdf5[cpp]"),
    [switch]$SkipClone,
    [switch]$SkipBootstrap,
    [switch]$CleanBuildtreesAfterInstall
)

$ErrorActionPreference = "Stop"

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

function Invoke-Step {
    param(
        [string]$Label,
        [scriptblock]$Action
    )

    Write-Host ""
    Write-Host "== $Label =="
    & $Action
}

$VcpkgRootPath = [System.IO.Path]::GetFullPath($VcpkgRoot)
$vcpkgExe = Join-Path $VcpkgRootPath "vcpkg.exe"
$bootstrapBat = Join-Path $VcpkgRootPath "bootstrap-vcpkg.bat"
$toolchainFile = Join-Path $VcpkgRootPath "scripts/buildsystems/vcpkg.cmake"
$binDir = Join-Path $VcpkgRootPath ("installed/" + $Triplet + "/bin")

Write-Host "Crimson Windows vcpkg setup"
Write-Host "  vcpkg root: $VcpkgRootPath"
Write-Host "  triplet:    $Triplet"
Write-Host "  packages:   $($Packages -join ', ')"

Invoke-Step "Tool check" {
    Require-Command git
}

if (-not (Test-Path -LiteralPath $VcpkgRootPath)) {
    if ($SkipClone) {
        throw "vcpkg root does not exist and -SkipClone was set: $VcpkgRootPath"
    }

    Invoke-Step "Clone vcpkg" {
        $parent = Split-Path -Parent $VcpkgRootPath
        if (-not (Test-Path -LiteralPath $parent)) {
            New-Item -ItemType Directory -Path $parent | Out-Null
        }
        Invoke-NativeCommand -Executable git -Arguments @("clone", $VcpkgRepository, $VcpkgRootPath)
    }
} else {
    Write-Host ""
    Write-Host "vcpkg root already exists: $VcpkgRootPath"
}

if (-not (Test-Path -LiteralPath $vcpkgExe)) {
    if ($SkipBootstrap) {
        throw "vcpkg.exe does not exist and -SkipBootstrap was set: $vcpkgExe"
    }

    Invoke-Step "Bootstrap vcpkg" {
        if (-not (Test-Path -LiteralPath $bootstrapBat)) {
            throw "bootstrap-vcpkg.bat not found: $bootstrapBat"
        }
        Invoke-NativeCommand -Executable $bootstrapBat
    }
} else {
    Write-Host "vcpkg.exe already exists: $vcpkgExe"
}

Invoke-Step "Install packages" {
    $installArgs = @("install")
    foreach ($package in $Packages) {
        if ($package -match ":") {
            $installArgs += $package
        } else {
            $installArgs += "${package}:$Triplet"
        }
    }
    Invoke-NativeCommand -Executable $vcpkgExe -Arguments $installArgs
}

if ($CleanBuildtreesAfterInstall) {
    Invoke-Step "Clean vcpkg buildtrees/packages" {
        foreach ($relativePath in @("buildtrees", "packages")) {
            $path = Join-Path $VcpkgRootPath $relativePath
            if (Test-Path -LiteralPath $path) {
                Remove-Item -LiteralPath $path -Recurse -Force
            }
        }
    }
}

Write-Host ""
Write-Host "Done."
Write-Host "  VcpkgRoot:       $VcpkgRootPath"
Write-Host "  VcpkgBinDir:     $binDir"
Write-Host "  Toolchain file:  $toolchainFile"
Write-Host ""
Write-Host "Next Crimson build command:"
Write-Host "  powershell -ExecutionPolicy Bypass -File .\tools\build_windows_app_drop.ps1 -VcpkgRoot `"$VcpkgRootPath`" -CleanInstall"

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

        if (Test-Path -LiteralPath $Root) {
            foreach ($child in (Get-ChildItem -LiteralPath $Root -Directory -ErrorAction SilentlyContinue)) {
                $candidates += @(
                    (Join-Path $child.FullName "lib/cmake/opencv4"),
                    (Join-Path $child.FullName "x64/vc17/lib"),
                    (Join-Path $child.FullName "build"),
                    $child.FullName
                )
            }
        }
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

function Resolve-VcpkgToolchainFile {
    param(
        [string]$Root,
        [string]$BinDir
    )

    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($Root)) {
        $candidates += (Join-Path $Root "scripts/buildsystems/vcpkg.cmake")
    }

    $cursor = $BinDir
    for ($i = 0; $i -lt 5; $i++) {
        if ([string]::IsNullOrWhiteSpace($cursor)) {
            break
        }
        $candidates += (Join-Path $cursor "scripts/buildsystems/vcpkg.cmake")
        $parent = Split-Path -Parent $cursor
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $cursor) {
            break
        }
        $cursor = $parent
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }

    return $null
}

function Resolve-Python3Executable {
    param(
        [string]$ExplicitExecutable
    )

    function Test-PythonCandidate {
        param([string]$Executable)

        if ([string]::IsNullOrWhiteSpace($Executable)) {
            return $null
        }

        try {
            $output = & $Executable -c "import sys; print(sys.executable)" 2>$null
            if ($LASTEXITCODE -eq 0 -and $output) {
                $resolved = ($output | Select-Object -First 1)
                if ($resolved -and (Test-Path -LiteralPath $resolved)) {
                    return [System.IO.Path]::GetFullPath($resolved)
                }
                return $Executable
            }
        } catch {
            # Fall through to the null result below.
        }

        return $null
    }

    if (-not [string]::IsNullOrWhiteSpace($ExplicitExecutable)) {
        if (Test-Path -LiteralPath $ExplicitExecutable) {
            $resolvedExplicit = Test-PythonCandidate -Executable ([System.IO.Path]::GetFullPath($ExplicitExecutable))
            if ($resolvedExplicit) {
                return $resolvedExplicit
            }
        }
        return Test-PythonCandidate -Executable $ExplicitExecutable
    }

    foreach ($name in @("python", "python3")) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command) {
            $resolved = Test-PythonCandidate -Executable $command.Source
            if ($resolved) {
                return $resolved
            }
        }
    }

    $pyLauncherCandidates = @()
    $pyLauncher = Get-Command py -ErrorAction SilentlyContinue
    if ($pyLauncher) {
        $pyLauncherCandidates += $pyLauncher.Source
    }
    if ($env:LOCALAPPDATA) {
        $pyLauncherCandidates += (Join-Path $env:LOCALAPPDATA "Programs/Python/Launcher/py.exe")
    }
    if ($env:SystemRoot) {
        $pyLauncherCandidates += (Join-Path $env:SystemRoot "py.exe")
    }
    if ($env:ProgramFiles) {
        $pyLauncherCandidates += (Join-Path $env:ProgramFiles "Python Launcher/py.exe")
    }

    foreach ($pyLauncherPath in ($pyLauncherCandidates | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $pyLauncherPath)) {
            continue
        }
        try {
            $output = & $pyLauncherPath -3 -c "import sys; print(sys.executable)" 2>$null
            if ($LASTEXITCODE -eq 0 -and $output) {
                $resolved = ($output | Select-Object -First 1)
                if ($resolved -and (Test-Path -LiteralPath $resolved)) {
                    return [System.IO.Path]::GetFullPath($resolved)
                }
            }
        } catch {
            # Fall through to the null result below.
        }
    }

    return $null
}

function Resolve-NasmExecutable {
    param(
        [string]$ExplicitExecutable
    )

    function Test-NasmCandidate {
        param([string]$Executable)

        if ([string]::IsNullOrWhiteSpace($Executable)) {
            return $null
        }
        if (-not (Test-Path -LiteralPath $Executable)) {
            return $null
        }

        try {
            & $Executable -v 2>$null | Out-Null
            if ($LASTEXITCODE -eq 0) {
                return [System.IO.Path]::GetFullPath($Executable)
            }
        } catch {
            # Fall through to the null result below.
        }

        return $null
    }

    if (-not [string]::IsNullOrWhiteSpace($ExplicitExecutable)) {
        $resolvedExplicit = Test-NasmCandidate -Executable $ExplicitExecutable
        if ($resolvedExplicit) {
            return $resolvedExplicit
        }
        return $ExplicitExecutable
    }

    if ($env:ASM_NASM) {
        $resolvedEnv = Test-NasmCandidate -Executable $env:ASM_NASM
        if ($resolvedEnv) {
            return $resolvedEnv
        }
    }

    $command = Get-Command nasm -ErrorAction SilentlyContinue
    if ($command) {
        $resolvedCommand = Test-NasmCandidate -Executable $command.Source
        if ($resolvedCommand) {
            return $resolvedCommand
        }
    }

    $candidates = @()
    if ($env:LOCALAPPDATA) {
        $candidates += (Join-Path $env:LOCALAPPDATA "bin/NASM/nasm.exe")
        $candidates += (Join-Path $env:LOCALAPPDATA "Programs/NASM/nasm.exe")
    }
    if ($env:ProgramFiles) {
        $candidates += (Join-Path $env:ProgramFiles "NASM/nasm.exe")
    }
    if (${env:ProgramFiles(x86)}) {
        $candidates += (Join-Path ${env:ProgramFiles(x86)} "NASM/nasm.exe")
    }

    foreach ($candidate in $candidates) {
        $resolved = Test-NasmCandidate -Executable $candidate
        if ($resolved) {
            return $resolved
        }
    }

    return $null
}

function Get-DefaultBuildDir {
    param(
        [string]$PresetName
    )

    $shortPresetName = switch ($PresetName) {
        "windows-trt10-cuda12.4-no-sfm" { "w124n"; break }
        "windows-trt10-cuda12.4" { "w124"; break }
        default {
            $sanitized = $PresetName -replace "[^A-Za-z0-9]+", ""
            if ($sanitized.Length -gt 24) {
                $sanitized = $sanitized.Substring(0, 24)
            }
            $sanitized.ToLowerInvariant()
        }
    }

    return Join-Path $RepoRoot ("build/" + $shortPresetName)
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

function Require-OneExistingPath {
    param(
        [string]$Label,
        [string[]]$Candidates,
        [string]$Hint
    )

    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        if (Test-Path -LiteralPath $candidate) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }

    Write-Host "$Label not found. Checked:"
    foreach ($candidate in $Candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate)) {
            Write-Host "  $candidate"
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($Hint)) {
        throw "$Label not found. $Hint"
    }
    throw "$Label not found."
}

function Require-FfmpegDevelopmentFiles {
    param(
        [string]$Root
    )

    Require-ExistingPath -Label "FFmpeg root" -PathValue $Root
    Require-ExistingPath -Label "FFmpeg avformat header" -PathValue (Join-Path $Root "include/libavformat/avformat.h")

    foreach ($libraryName in @("avformat", "avcodec", "avutil", "swscale", "swresample")) {
        Require-ExistingPath `
            -Label "FFmpeg $libraryName import library" `
            -PathValue (Join-Path $Root ("lib/" + $libraryName + ".lib"))
    }
}

function Require-NvidiaCodecImportLibrary {
    param(
        [string]$LibraryName,
        [string]$VideoCodecSdkRootValue
    )

    $candidates = @(
        (Join-Path $RepoRoot ("third_party/nvcodec/x64/" + $LibraryName + ".lib"))
    )

    if (-not [string]::IsNullOrWhiteSpace($VideoCodecSdkRootValue)) {
        $candidates += @(
            (Join-Path $VideoCodecSdkRootValue ("Lib/win/x64/" + $LibraryName + ".lib")),
            (Join-Path $VideoCodecSdkRootValue ("lib/win/x64/" + $LibraryName + ".lib")),
            (Join-Path $VideoCodecSdkRootValue ("Lib/x64/" + $LibraryName + ".lib")),
            (Join-Path $VideoCodecSdkRootValue ("lib/x64/" + $LibraryName + ".lib")),
            (Join-Path $VideoCodecSdkRootValue ("Lib/" + $LibraryName + ".lib")),
            (Join-Path $VideoCodecSdkRootValue ("lib/" + $LibraryName + ".lib"))
        )
    }

    $resolvedLibrary = Require-OneExistingPath `
        -Label "NVIDIA $LibraryName import library" `
        -Candidates $candidates `
        -Hint "Install/extract the NVIDIA Video Codec SDK or keep the repo's third_party\nvcodec import libraries available."
    return $resolvedLibrary
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
if ([string]::IsNullOrWhiteSpace($VcpkgBinDir)) {
    $VcpkgBinDir = Join-Path $VcpkgRoot ("installed/" + $VcpkgTriplet + "/bin")
}
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Get-DefaultBuildDir -PresetName $Preset
}

$OpenCvDir = Resolve-OpenCvConfigDir -ExplicitConfigDir $OpenCvDir -Root $OpenCvRoot
$VcpkgToolchainFile = Resolve-VcpkgToolchainFile -Root $VcpkgRoot -BinDir $VcpkgBinDir
$Python3ExecutablePath = Resolve-Python3Executable -ExplicitExecutable $Python3Executable
$NasmExecutablePath = Resolve-NasmExecutable -ExplicitExecutable $NasmExecutable
$InstallPrefixPath = if ([System.IO.Path]::IsPathRooted($InstallPrefix)) {
    [System.IO.Path]::GetFullPath($InstallPrefix)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $InstallPrefix))
}
$BuildDirPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) {
    [System.IO.Path]::GetFullPath($BuildDir)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $BuildDir))
}

Write-Host "Crimson Windows app-drop build"
Write-Host "  repo:          $RepoRoot"
Write-Host "  preset:        $Preset"
Write-Host "  config:        $Configuration"
Write-Host "  build dir:     $BuildDirPath"
Write-Host "  install root:  $InstallPrefixPath"

Invoke-Step "Tool check" {
    Require-Command cmake
    Require-Command git
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
        Write-Host "ninja was not found in PATH. This is OK only if CMake can still find Ninja from the active Visual Studio developer shell."
    }
    if (-not $Python3ExecutablePath) {
        throw "Python 3 was not found. Install Python 3 or pass -Python3Executable to this script. TensorStore requires Python during CMake configure."
    }
    if (-not $NasmExecutablePath) {
        throw "NASM was not found. Install NASM or pass -NasmExecutable to this script. TensorStore requires NASM during CMake configure."
    }
    Invoke-NativeCommand -Executable cmake -Arguments @("--version")
    Invoke-NativeCommand -Executable $Python3ExecutablePath -Arguments @("--version")
    Invoke-NativeCommand -Executable $NasmExecutablePath -Arguments @("-v")
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
        Require-ExistingPath -Label "OpenCV CMake config" -PathValue (Join-Path $OpenCvDir "OpenCVConfig.cmake")
        Require-ExistingPath -Label "TensorRT root" -PathValue $TensorRtRoot
        Require-FfmpegDevelopmentFiles -Root $FfmpegRoot
        Require-NvidiaCodecImportLibrary -LibraryName "nvcuvid" -VideoCodecSdkRootValue $VideoCodecSdkRoot | Out-Null
        if ($VcpkgToolchainFile) {
            Write-Host "Vcpkg toolchain: $VcpkgToolchainFile"
        } else {
            Write-Warning "Vcpkg toolchain not found. GLEW, glfw3, zlib, and HDF5 must be discoverable by another CMake search path. To install the default source-build packages, run: powershell -ExecutionPolicy Bypass -File .\tools\setup_windows_vcpkg.ps1"
        }
        Write-Host "Python 3: $Python3ExecutablePath"
        Write-Host "NASM: $NasmExecutablePath"
    }
}

if (-not $SkipConfigure) {
    Invoke-Step "Configure" {
        $configureArgs = @("--preset", $Preset, "-S", $RepoRoot, "-B", $BuildDirPath)
        if ($VcpkgToolchainFile) {
            $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchainFile"
            $configureArgs += "-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet"
        }
        if ($Python3ExecutablePath) {
            $configureArgs += "-DPython3_EXECUTABLE=$Python3ExecutablePath"
        }
        if ($NasmExecutablePath) {
            $configureArgs += "-DCMAKE_ASM_NASM_COMPILER=$NasmExecutablePath"
        }
        Invoke-NativeCommand -Executable cmake -Arguments $configureArgs
    }
}

if (-not $SkipBuild) {
    Invoke-Step "Build" {
        Invoke-NativeCommand -Executable cmake -Arguments @("--build", $BuildDirPath, "--config", $Configuration)
    }
}

if (-not $SkipInstall) {
    Invoke-Step "Install app drop" {
        if ($CleanInstall -and (Test-Path -LiteralPath $InstallPrefixPath)) {
            Remove-Item -LiteralPath $InstallPrefixPath -Recurse -Force
        }
        Invoke-NativeCommand -Executable cmake -Arguments @("--install", $BuildDirPath, "--config", $Configuration, "--prefix", $InstallPrefixPath)
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

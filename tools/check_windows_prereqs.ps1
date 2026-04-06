[CmdletBinding()]
param(
    [string]$RepoRoot,
    [string]$CudaToolkitRoot = $(if ($env:CRIMSON_CUDA_TOOLKIT_ROOT) { $env:CRIMSON_CUDA_TOOLKIT_ROOT } else { "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.4" }),
    [string]$OpenCvDir = $(if ($env:CRIMSON_OPENCV_DIR) { $env:CRIMSON_OPENCV_DIR } else { "C:/third_party/opencv-install-4.10.0-x64" }),
    [string]$FfmpegRoot = $(if ($env:CRIMSON_FFMPEG_ROOT) { $env:CRIMSON_FFMPEG_ROOT } else { "C:/third_party/ffmpeg-nvidia" }),
    [string]$VideoCodecSdkRoot = $(if ($env:CRIMSON_VIDEO_CODEC_SDK_ROOT) { $env:CRIMSON_VIDEO_CODEC_SDK_ROOT } else { "C:/third_party/Video_Codec_SDK_13.0" }),
    [string]$TensorRtRoot = $(if ($env:CRIMSON_TENSORRT_ROOT) { $env:CRIMSON_TENSORRT_ROOT } else { "C:/third_party/TensorRT-10.0.1.6" }),
    [string]$VcpkgBinDir = "C:/src/vcpkg/installed/x64-windows/bin"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
}

$results = New-Object System.Collections.Generic.List[object]

function Add-Result {
    param(
        [string]$Category,
        [string]$Name,
        [bool]$Ok,
        [string]$Details
    )

    $status = if ($Ok) { "OK" } else { "FAIL" }
    [void]$results.Add([PSCustomObject]@{
        Category = $Category
        Name = $Name
        Status = $status
        Details = $Details
    })
}

function Test-Command {
    param(
        [string]$Name,
        [string[]]$VersionArgs = @("--version"),
        [string]$FriendlyName = $Name
    )

    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $cmd) {
        Add-Result -Category "command" -Name $FriendlyName -Ok $false -Details "not found in PATH"
        return
    }

    $detail = $cmd.Source
    if ($VersionArgs.Count -gt 0) {
        try {
            $output = & $cmd.Source @VersionArgs 2>&1
            if ($LASTEXITCODE -eq 0 -or $null -eq $LASTEXITCODE) {
                $firstLine = ($output | Select-Object -First 1)
                if ($firstLine) {
                    $detail = "$detail | $firstLine"
                }
            } else {
                $detail = "$detail | version probe failed"
            }
        } catch {
            $detail = "$detail | version probe failed"
        }
    }

    Add-Result -Category "command" -Name $FriendlyName -Ok $true -Details $detail
}

function Test-PathExists {
    param(
        [string]$Label,
        [string]$PathValue,
        [string]$Category = "path"
    )

    $exists = Test-Path -LiteralPath $PathValue
    $detail = if ($exists) { $PathValue } else { "missing: $PathValue" }
    Add-Result -Category $Category -Name $Label -Ok $exists -Details $detail
    return $exists
}

function Find-FirstExistingPath {
    param(
        [string[]]$Candidates
    )

    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    return $null
}

function Test-RequiredFile {
    param(
        [string]$Label,
        [string[]]$Candidates,
        [string]$Category = "file"
    )

    $resolved = Find-FirstExistingPath -Candidates $Candidates
    if ($resolved) {
        Add-Result -Category $Category -Name $Label -Ok $true -Details $resolved
        return $true
    }

    $detail = "missing: " + ($Candidates -join " | ")
    Add-Result -Category $Category -Name $Label -Ok $false -Details $detail
    return $false
}

function Test-VisualStudioToolchain {
    $vswhereCandidates = @()
    if ($env:ProgramFiles) {
        $vswhereCandidates += (Join-Path $env:ProgramFiles "Microsoft Visual Studio/Installer/vswhere.exe")
    }
    if (${env:ProgramFiles(x86)}) {
        $vswhereCandidates += (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe")
    }

    $vswhere = Find-FirstExistingPath -Candidates $vswhereCandidates
    if ($vswhere) {
        try {
            $json = & $vswhere `
                -latest `
                -products * `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                -format json `
                -utf8 2>$null

            if ($LASTEXITCODE -eq 0 -and $json) {
                $installations = @($json | ConvertFrom-Json)
                if ($installations.Count -gt 0) {
                    $install = $installations[0]
                    $displayName = if ($install.displayName) { $install.displayName } else { $install.productId }
                    $installVersion = if ($install.catalog -and $install.catalog.productDisplayVersion) {
                        $install.catalog.productDisplayVersion
                    } elseif ($install.installationVersion) {
                        $install.installationVersion
                    } else {
                        "unknown-version"
                    }
                    $detail = "$displayName | $installVersion | $($install.installationPath)"
                    Add-Result -Category "visual_studio" -Name "Visual Studio C++ toolchain" -Ok $true -Details $detail
                    return
                }
            }
        } catch {
            # Fall through to path-based checks below.
        }

    }

    $devShellCandidates = @()
    foreach ($edition in @("BuildTools", "Community", "Professional", "Enterprise")) {
        if (${env:ProgramFiles(x86)}) {
            $devShellCandidates += (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/2022/$edition/Common7/Tools/VsDevCmd.bat")
            $devShellCandidates += (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/2022/$edition/VC/Auxiliary/Build/vcvars64.bat")
        }
        if ($env:ProgramFiles) {
            $devShellCandidates += (Join-Path $env:ProgramFiles "Microsoft Visual Studio/2022/$edition/Common7/Tools/VsDevCmd.bat")
            $devShellCandidates += (Join-Path $env:ProgramFiles "Microsoft Visual Studio/2022/$edition/VC/Auxiliary/Build/vcvars64.bat")
        }
    }

    $resolvedDevShell = Find-FirstExistingPath -Candidates $devShellCandidates
    if ($resolvedDevShell) {
        Add-Result -Category "visual_studio" -Name "Visual Studio developer shell scripts" -Ok $true -Details $resolvedDevShell
        return
    }

    $detail = if ($vswhere) {
        "No Visual Studio 2022 Build Tools / C++ workload detected via vswhere or standard vcvars paths"
    } else {
        "vswhere.exe missing and no standard Visual Studio 2022 vcvars paths were found"
    }
    Add-Result -Category "visual_studio" -Name "Visual Studio C++ toolchain" -Ok $false -Details $detail
}

function Test-VideoCodecSdk {
    param(
        [string]$Root
    )

    $rootOk = Test-PathExists -Label "CRIMSON_VIDEO_CODEC_SDK_ROOT" -PathValue $Root -Category "path"
    if (-not $rootOk) {
        return
    }

    Test-RequiredFile -Label "Video Codec SDK Interface" -Candidates @(
        (Join-Path $Root "Interface")
    ) -Category "sdk"

    Test-RequiredFile -Label "Video Codec SDK lib dir" -Candidates @(
        (Join-Path $Root "Lib/x64"),
        (Join-Path $Root "Lib/win/x64")
    ) -Category "sdk"
}

function Test-OpenCv {
    param(
        [string]$Root
    )

    $rootOk = Test-PathExists -Label "CRIMSON_OPENCV_DIR" -PathValue $Root -Category "path"
    if (-not $rootOk) {
        return
    }

    Test-RequiredFile -Label "OpenCVConfig.cmake" -Candidates @(
        (Join-Path $Root "OpenCVConfig.cmake"),
        (Join-Path $Root "x64/vc17/lib/OpenCVConfig.cmake"),
        (Join-Path $Root "lib/OpenCVConfig.cmake")
    ) -Category "opencv"

    Test-RequiredFile -Label "OpenCV runtime bin dir" -Candidates @(
        (Join-Path $Root "x64/vc17/bin"),
        (Join-Path (Split-Path -Parent $Root) "bin")
    ) -Category "opencv"
}

function Test-Ffmpeg {
    param(
        [string]$Root
    )

    $rootOk = Test-PathExists -Label "CRIMSON_FFMPEG_ROOT" -PathValue $Root -Category "path"
    if (-not $rootOk) {
        return
    }

    Test-RequiredFile -Label "FFmpeg include dir" -Candidates @(
        (Join-Path $Root "include")
    ) -Category "ffmpeg"

    Test-RequiredFile -Label "FFmpeg lib dir" -Candidates @(
        (Join-Path $Root "lib")
    ) -Category "ffmpeg"

    Test-RequiredFile -Label "ffmpeg.exe" -Candidates @(
        (Join-Path $Root "bin/ffmpeg.exe")
    ) -Category "ffmpeg"

    Test-RequiredFile -Label "ffprobe.exe" -Candidates @(
        (Join-Path $Root "bin/ffprobe.exe")
    ) -Category "ffmpeg"

    foreach ($libName in @("avcodec.lib", "avformat.lib", "avutil.lib", "swscale.lib", "swresample.lib")) {
        Test-RequiredFile -Label "FFmpeg import lib $libName" -Candidates @(
            (Join-Path $Root "lib/$libName")
        ) -Category "ffmpeg"
    }
}

function Test-TensorRt {
    param(
        [string]$Root
    )

    $rootOk = Test-PathExists -Label "CRIMSON_TENSORRT_ROOT" -PathValue $Root -Category "path"
    if (-not $rootOk) {
        return
    }

    Test-RequiredFile -Label "NvInfer.h" -Candidates @(
        (Join-Path $Root "include/NvInfer.h")
    ) -Category "tensorrt"

    Test-RequiredFile -Label "NvInferVersion.h" -Candidates @(
        (Join-Path $Root "include/NvInferVersion.h")
    ) -Category "tensorrt"

    Test-RequiredFile -Label "TensorRT lib dir" -Candidates @(
        (Join-Path $Root "lib"),
        (Join-Path $Root "lib/x64")
    ) -Category "tensorrt"

    Test-RequiredFile -Label "TensorRT bin dir" -Candidates @(
        (Join-Path $Root "bin")
    ) -Category "tensorrt"
}

function Test-CudaToolkit {
    param(
        [string]$Root
    )

    $rootOk = Test-PathExists -Label "CRIMSON_CUDA_TOOLKIT_ROOT" -PathValue $Root -Category "path"
    if (-not $rootOk) {
        return
    }

    Test-RequiredFile -Label "nvcc.exe" -Candidates @(
        (Join-Path $Root "bin/nvcc.exe")
    ) -Category "cuda"

    Test-RequiredFile -Label "cuda.h" -Candidates @(
        (Join-Path $Root "include/cuda.h")
    ) -Category "cuda"
}

function Test-RepoLayout {
    param(
        [string]$Root
    )

    $rootOk = Test-PathExists -Label "Repo root" -PathValue $Root -Category "repo"
    if (-not $rootOk) {
        return
    }

    Test-RequiredFile -Label "CMakePresets.json" -Candidates @(
        (Join-Path $Root "CMakePresets.json")
    ) -Category "repo"

    foreach ($submodulePath in @(
        "third_party/imgui",
        "third_party/implot",
        "third_party/ImGuiFileDialog",
        "third_party/IconFontCppHeaders"
    )) {
        Test-RequiredFile -Label $submodulePath -Candidates @(
            (Join-Path $Root $submodulePath)
        ) -Category "repo"
    }
}

Write-Host "Checking Crimson Windows prerequisites..."
Write-Host ""

Test-VisualStudioToolchain
Test-Command -Name "git" -FriendlyName "Git"
Test-Command -Name "cmake" -FriendlyName "CMake"
Test-Command -Name "ninja" -FriendlyName "Ninja"
Test-Command -Name "cl" -VersionArgs @() -FriendlyName "MSVC cl.exe"
Test-Command -Name "nvidia-smi" -VersionArgs @() -FriendlyName "NVIDIA driver / nvidia-smi"
Test-Command -Name "nvcc" -FriendlyName "CUDA nvcc"

Test-RepoLayout -Root $RepoRoot
Test-CudaToolkit -Root $CudaToolkitRoot
Test-OpenCv -Root $OpenCvDir
Test-Ffmpeg -Root $FfmpegRoot
Test-VideoCodecSdk -Root $VideoCodecSdkRoot
Test-TensorRt -Root $TensorRtRoot
Test-PathExists -Label "VCPKG_BIN_DIR" -PathValue $VcpkgBinDir -Category "path" | Out-Null

$results |
    Sort-Object Category, Name |
    Format-Table -AutoSize Category, Name, Status, Details

$failCount = @($results | Where-Object { $_.Status -eq "FAIL" }).Count

Write-Host ""
if ($failCount -eq 0) {
    Write-Host "All prerequisite checks passed."
    Write-Host "Suggested next step:"
    Write-Host "  . .\\tools\\set_windows_dependency_roots.ps1"
    Write-Host "  cmake --preset windows-trt10-cuda12.4-no-sfm"
    exit 0
}

Write-Host "$failCount prerequisite check(s) failed."
Write-Host "Fix the failing items above, then rerun this script."
Write-Host "Common causes:"
Write-Host "  - not using Developer PowerShell / Native Tools shell"
Write-Host "  - dependency roots still pointed at placeholder defaults"
Write-Host "  - FFmpeg not yet staged into Crimson's expected include/lib/bin layout"
exit 1

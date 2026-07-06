[CmdletBinding()]
param(
    [string]$ThirdPartyRoot = "C:\third_party",
    [string]$FfmpegRoot,
    [string]$VideoCodecSdkRoot,
    [string]$MediaAutobuildRoot = "C:\src\media-autobuild_suite\local64",
    [switch]$CheckMediaAutobuild,
    [switch]$Json
)

$ErrorActionPreference = "Stop"

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

if ([string]::IsNullOrWhiteSpace($FfmpegRoot)) {
    $FfmpegRoot = Join-Path $ThirdPartyRoot "ffmpeg-nvidia"
}
if ([string]::IsNullOrWhiteSpace($VideoCodecSdkRoot)) {
    $VideoCodecSdkRoot = Join-Path $ThirdPartyRoot "Video_Codec_SDK_13.0.19"
}

$results = New-Object System.Collections.Generic.List[object]
$hasFailures = $false
$hasWarnings = $false

function Add-Result {
    param(
        [string]$Category,
        [string]$Name,
        [string]$Status,
        [string]$Details
    )

    if ($Status -eq "FAIL") {
        $script:hasFailures = $true
    } elseif ($Status -eq "WARN") {
        $script:hasWarnings = $true
    }

    [void]$script:results.Add([PSCustomObject]@{
        Category = $Category
        Name = $Name
        Status = $Status
        Details = $Details
    })
}

function Add-Ok { param([string]$Category, [string]$Name, [string]$Details) Add-Result $Category $Name "OK" $Details }
function Add-Warn { param([string]$Category, [string]$Name, [string]$Details) Add-Result $Category $Name "WARN" $Details }
function Add-Fail { param([string]$Category, [string]$Name, [string]$Details) Add-Result $Category $Name "FAIL" $Details }

function Test-RequiredPath {
    param(
        [string]$Category,
        [string]$Name,
        [string]$PathValue
    )

    if (Test-Path -LiteralPath $PathValue) {
        Add-Ok -Category $Category -Name $Name -Details $PathValue
        return $true
    }

    Add-Fail -Category $Category -Name $Name -Details "missing: $PathValue"
    return $false
}

function Test-OptionalPath {
    param(
        [string]$Category,
        [string]$Name,
        [string]$PathValue
    )

    if (Test-Path -LiteralPath $PathValue) {
        Add-Ok -Category $Category -Name $Name -Details $PathValue
        return $true
    }

    Add-Warn -Category $Category -Name $Name -Details "missing: $PathValue"
    return $false
}

function Test-OneRequiredPath {
    param(
        [string]$Category,
        [string]$Name,
        [string[]]$Candidates
    )

    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        if (Test-Path -LiteralPath $candidate) {
            Add-Ok -Category $Category -Name $Name -Details $candidate
            return $true
        }
    }

    $checked = ($Candidates | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) -join "; "
    Add-Fail -Category $Category -Name $Name -Details "missing; checked: $checked"
    return $false
}

function Get-NvidiaCodecLibraryCandidates {
    param(
        [string]$LibraryName
    )

    $candidates = @(
        (Join-Path $RepoRoot ("third_party\nvcodec\x64\" + $LibraryName + ".lib"))
    )

    if (-not [string]::IsNullOrWhiteSpace($VideoCodecSdkRoot)) {
        $candidates += @(
            (Join-Path $VideoCodecSdkRoot ("Lib\win\x64\" + $LibraryName + ".lib")),
            (Join-Path $VideoCodecSdkRoot ("lib\win\x64\" + $LibraryName + ".lib")),
            (Join-Path $VideoCodecSdkRoot ("Lib\x64\" + $LibraryName + ".lib")),
            (Join-Path $VideoCodecSdkRoot ("lib\x64\" + $LibraryName + ".lib")),
            (Join-Path $VideoCodecSdkRoot ("Lib\" + $LibraryName + ".lib")),
            (Join-Path $VideoCodecSdkRoot ("lib\" + $LibraryName + ".lib"))
        )
    }

    return $candidates
}

Test-RequiredPath -Category "ffmpeg" -Name "FFmpeg root" -PathValue $FfmpegRoot | Out-Null
Test-RequiredPath -Category "ffmpeg" -Name "avformat header" -PathValue (Join-Path $FfmpegRoot "include\libavformat\avformat.h") | Out-Null

foreach ($libraryName in @("avformat", "avcodec", "avutil", "swscale", "swresample")) {
    Test-RequiredPath `
        -Category "ffmpeg" `
        -Name "$libraryName import library" `
        -PathValue (Join-Path $FfmpegRoot ("lib\" + $libraryName + ".lib")) | Out-Null
}

Test-OptionalPath -Category "ffmpeg" -Name "ffmpeg.exe" -PathValue (Join-Path $FfmpegRoot "bin\ffmpeg.exe") | Out-Null
Test-OptionalPath -Category "ffmpeg" -Name "ffprobe.exe" -PathValue (Join-Path $FfmpegRoot "bin\ffprobe.exe") | Out-Null

$ffmpegBin = Join-Path $FfmpegRoot "bin"
if (Test-Path -LiteralPath $ffmpegBin) {
    $dllCount = @(Get-ChildItem -LiteralPath $ffmpegBin -Filter "*.dll" -File -ErrorAction SilentlyContinue).Count
    if ($dllCount -gt 0) {
        Add-Ok -Category "ffmpeg" -Name "runtime DLLs" -Details "$dllCount DLLs in $ffmpegBin"
    } else {
        Add-Warn -Category "ffmpeg" -Name "runtime DLLs" -Details "no DLLs found in $ffmpegBin"
    }
}

Test-OptionalPath -Category "nvidia-codec" -Name "Video Codec SDK root" -PathValue $VideoCodecSdkRoot | Out-Null
Test-OptionalPath -Category "nvidia-codec" -Name "SDK Interface directory" -PathValue (Join-Path $VideoCodecSdkRoot "Interface") | Out-Null
Test-RequiredPath -Category "nvidia-codec" -Name "vendored nvcodec headers" -PathValue (Join-Path $RepoRoot "third_party\nvcodec\nvcuvid.h") | Out-Null
Test-OneRequiredPath -Category "nvidia-codec" -Name "nvcuvid import library" -Candidates (Get-NvidiaCodecLibraryCandidates -LibraryName "nvcuvid") | Out-Null
Test-OneRequiredPath -Category "nvidia-codec" -Name "nvencodeapi import library" -Candidates (Get-NvidiaCodecLibraryCandidates -LibraryName "nvencodeapi") | Out-Null

if ($CheckMediaAutobuild) {
    Test-RequiredPath -Category "media-autobuild" -Name "local64 root" -PathValue $MediaAutobuildRoot | Out-Null
    Test-RequiredPath -Category "media-autobuild" -Name "include directory" -PathValue (Join-Path $MediaAutobuildRoot "include") | Out-Null
    Test-RequiredPath -Category "media-autobuild" -Name "lib directory" -PathValue (Join-Path $MediaAutobuildRoot "lib") | Out-Null
    Test-RequiredPath -Category "media-autobuild" -Name "bin-video directory" -PathValue (Join-Path $MediaAutobuildRoot "bin-video") | Out-Null
    Test-RequiredPath -Category "media-autobuild" -Name "source ffmpeg.exe" -PathValue (Join-Path $MediaAutobuildRoot "bin-video\ffmpeg.exe") | Out-Null
    foreach ($definitionPattern in @("avformat-*.def", "avcodec-*.def", "avutil-*.def", "swscale-*.def", "swresample-*.def")) {
        $matches = @(
            Get-ChildItem -LiteralPath (Join-Path $MediaAutobuildRoot "lib") `
                -Filter $definitionPattern `
                -File `
                -ErrorAction SilentlyContinue
        )
        if ($matches.Count -eq 1) {
            Add-Ok -Category "media-autobuild" -Name $definitionPattern -Details $matches[0].FullName
        } elseif ($matches.Count -gt 1) {
            $names = ($matches | Sort-Object Name | ForEach-Object { $_.FullName }) -join "; "
            Add-Fail -Category "media-autobuild" -Name $definitionPattern -Details "ambiguous: $names"
        } else {
            Add-Fail -Category "media-autobuild" -Name $definitionPattern -Details "missing in $(Join-Path $MediaAutobuildRoot 'lib')"
        }
    }
}

if ($Json) {
    [PSCustomObject]@{
        ok = -not $hasFailures
        has_warnings = $hasWarnings
        ffmpeg_root = $FfmpegRoot
        video_codec_sdk_root = $VideoCodecSdkRoot
        repo_root = $RepoRoot
        results = $results
    } | ConvertTo-Json -Depth 5
} else {
    Write-Host "Crimson Windows build dependency path check"
    Write-Host "  repo:             $RepoRoot"
    Write-Host "  FFmpeg root:      $FfmpegRoot"
    Write-Host "  Video Codec SDK:  $VideoCodecSdkRoot"
    if ($CheckMediaAutobuild) {
        Write-Host "  Media Autobuild:  $MediaAutobuildRoot"
    }
    Write-Host ""

    $results | Format-Table -AutoSize Category, Name, Status, Details

    if ($hasFailures) {
        Write-Host ""
        Write-Host "Missing required build inputs."
        Write-Host ""
        Write-Host "If FFmpeg files are missing and Media Autobuild Suite is available, run:"
        Write-Host "  powershell -ExecutionPolicy Bypass -File .\tools\stage_windows_ffmpeg_nvidia.ps1 ``"
        Write-Host "    -MediaAutobuildRoot $MediaAutobuildRoot ``"
        Write-Host "    -OutputRoot $FfmpegRoot ``"
        Write-Host "    -CleanOutput"
    } else {
        Write-Host ""
        Write-Host "All required FFmpeg/NVIDIA codec build paths are present."
    }
}

if ($hasFailures) {
    exit 1
}

exit 0

[CmdletBinding()]
param(
    [string]$MediaAutobuildRoot = "C:\src\media-autobuild_suite\local64",
    [string]$OutputRoot = "C:\third_party\ffmpeg-nvidia",
    [switch]$CleanOutput
)

$ErrorActionPreference = "Stop"

function Require-Path {
    param(
        [string]$Path,
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Description not found: $Path"
    }
}

function Resolve-SingleFile {
    param(
        [string]$Directory,
        [string]$Pattern,
        [string]$Description
    )

    $matches = @(Get-ChildItem -LiteralPath $Directory -Filter $Pattern -File -ErrorAction Stop)
    if ($matches.Count -eq 0) {
        throw "$Description not found in $Directory using pattern $Pattern"
    }
    if ($matches.Count -gt 1) {
        $names = $matches | Sort-Object Name | ForEach-Object { $_.Name }
        throw "${Description} is ambiguous in ${Directory}: $($names -join ', ')"
    }
    return $matches[0].FullName
}

function Invoke-LibImport {
    param(
        [string]$DefinitionFile,
        [string]$OutputLibrary
    )

    Write-Host "Generating $(Split-Path -Leaf $OutputLibrary) from $(Split-Path -Leaf $DefinitionFile)"
    & lib.exe "/def:$DefinitionFile" "/machine:x64" "/out:$OutputLibrary"
    if ($LASTEXITCODE -ne 0) {
        throw "lib.exe failed for $DefinitionFile"
    }
}

function Copy-DirectoryContents {
    param(
        [string]$SourceDirectory,
        [string]$DestinationDirectory
    )

    New-Item -ItemType Directory -Force -Path $DestinationDirectory | Out-Null
    Copy-Item -Path (Join-Path $SourceDirectory "*") -Destination $DestinationDirectory -Recurse -Force
}

$libCommand = Get-Command lib.exe -ErrorAction SilentlyContinue
if (-not $libCommand) {
    throw "lib.exe not found. Run this script from Developer PowerShell for VS 2022 or another MSVC developer shell."
}

$includeDir = Join-Path $MediaAutobuildRoot "include"
$libDir = Join-Path $MediaAutobuildRoot "lib"
$binDir = Join-Path $MediaAutobuildRoot "bin-video"
$ffmpegExe = Join-Path $binDir "ffmpeg.exe"
$ffprobeExe = Join-Path $binDir "ffprobe.exe"

Require-Path $MediaAutobuildRoot "Media Autobuild Suite local64 root"
Require-Path $includeDir "FFmpeg include directory"
Require-Path $libDir "FFmpeg library directory"
Require-Path $binDir "FFmpeg bin-video directory"
Require-Path $ffmpegExe "ffmpeg.exe"

if ($CleanOutput -and (Test-Path -LiteralPath $OutputRoot)) {
    Write-Host "Removing existing output root: $OutputRoot"
    Remove-Item -LiteralPath $OutputRoot -Recurse -Force
}

$destIncludeDir = Join-Path $OutputRoot "include"
$destLibDir = Join-Path $OutputRoot "lib"
$destBinDir = Join-Path $OutputRoot "bin"

New-Item -ItemType Directory -Force -Path $destIncludeDir | Out-Null
New-Item -ItemType Directory -Force -Path $destLibDir | Out-Null
New-Item -ItemType Directory -Force -Path $destBinDir | Out-Null

Write-Host "Validating FFmpeg binary:"
& $ffmpegExe -hide_banner -version
if ($LASTEXITCODE -ne 0) {
    throw "ffmpeg.exe -version failed"
}

$hwaccels = & $ffmpegExe -hide_banner -hwaccels
if ($LASTEXITCODE -ne 0) {
    throw "ffmpeg.exe -hwaccels failed"
}
Write-Host $hwaccels
if (-not ($hwaccels | Select-String -SimpleMatch "cuda")) {
    throw "FFmpeg hardware acceleration list does not contain CUDA."
}

$encoders = & $ffmpegExe -hide_banner -encoders
if ($LASTEXITCODE -ne 0) {
    throw "ffmpeg.exe -encoders failed"
}
$nvencEncoders = $encoders | Select-String "nvenc"
if (-not $nvencEncoders) {
    throw "FFmpeg encoder list does not contain NVIDIA NVENC entries."
}
Write-Host $nvencEncoders

Write-Host "Staging headers and runtime binaries into $OutputRoot"
Copy-DirectoryContents -SourceDirectory $includeDir -DestinationDirectory $destIncludeDir

Get-ChildItem -LiteralPath $binDir -File |
    Where-Object { $_.Extension -eq ".dll" -or $_.Name -in @("ffmpeg.exe", "ffprobe.exe") } |
    Copy-Item -Destination $destBinDir -Force

Get-ChildItem -LiteralPath $libDir -Filter "*.def" -File | Copy-Item -Destination $destLibDir -Force

$definitionMap = [ordered]@{
    "avcodec"    = "avcodec-*.def"
    "avformat"   = "avformat-*.def"
    "avutil"     = "avutil-*.def"
    "swscale"    = "swscale-*.def"
    "swresample" = "swresample-*.def"
}

foreach ($libraryName in $definitionMap.Keys) {
    $definitionFile = Resolve-SingleFile -Directory $libDir -Pattern $definitionMap[$libraryName] -Description "$libraryName import definition"
    $outputLibrary = Join-Path $destLibDir "$libraryName.lib"
    Invoke-LibImport -DefinitionFile $definitionFile -OutputLibrary $outputLibrary
}

Write-Host "Generated import libraries:"
Get-ChildItem -LiteralPath $destLibDir -Filter "*.lib" | Sort-Object Name | Select-Object Name, Length

Write-Host ""
Write-Host "FFmpeg staging complete."
Write-Host "Set CRIMSON_FFMPEG_ROOT to: $OutputRoot"
if (Test-Path -LiteralPath $ffprobeExe) {
    Write-Host "Optional verification:"
    Write-Host "  $OutputRoot\\bin\\ffmpeg.exe -hide_banner -hwaccels"
    Write-Host "  $OutputRoot\\bin\\ffmpeg.exe -hide_banner -encoders | findstr nvenc"
    Write-Host "  $OutputRoot\\bin\\ffprobe.exe -hide_banner -version"
}

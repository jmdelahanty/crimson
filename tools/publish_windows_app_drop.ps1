[CmdletBinding()]
param(
    [string]$RepoRoot,
    [string]$StageRoot,
    [string]$ShareRoot = "\\YOUR-SERVER\crimson\windows-app",
    [string]$DropName = "current",
    [switch]$CleanDestination,
    [string]$ReleaseName,
    [switch]$PublishCurrent,
    [switch]$ArchiveExistingCurrent,
    [string]$CurrentName = "current",
    [string]$ReleasesDirName = "releases"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
}

if ([string]::IsNullOrWhiteSpace($StageRoot)) {
    $StageRoot = Join-Path $RepoRoot "dist/Crimson"
}

function Require-Path {
    param(
        [string]$PathValue,
        [string]$Label
    )

    if (-not (Test-Path -LiteralPath $PathValue)) {
        throw "$Label not found: $PathValue"
    }

    return $PathValue
}

function Assert-MissingPath {
    param(
        [string]$PathValue,
        [string]$Label
    )

    if (Test-Path -LiteralPath $PathValue) {
        throw "$Label should not exist: $PathValue`nClean the staged install tree and reinstall so the Windows app layout is consistent."
    }
}

function Copy-AppTree {
    param(
        [string]$SourceRoot,
        [string]$TargetRoot,
        [string]$Label,
        [switch]$CleanTarget
    )

    if ($CleanTarget -and (Test-Path -LiteralPath $TargetRoot)) {
        Write-Host "Removing existing ${Label}:"
        Write-Host "  $TargetRoot"
        Remove-Item -LiteralPath $TargetRoot -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path $TargetRoot | Out-Null
    Write-Host "Copying ${Label}:"
    Write-Host "  from: $SourceRoot"
    Write-Host "  to:   $TargetRoot"
    Copy-Item -Path (Join-Path $SourceRoot "*") -Destination $TargetRoot -Recurse -Force
}

function Get-UtcTimestampString {
    return (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
}

function Write-JsonFile {
    param(
        [string]$PathValue,
        [object]$Data
    )

    $parent = Split-Path -Parent $PathValue
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }

    $json = $Data | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText(
        $PathValue,
        $json + [Environment]::NewLine,
        [System.Text.UTF8Encoding]::new($false))
}

function Try-GetGitString {
    param([string[]]$Arguments)

    try {
        $result = & git -C $RepoRoot @Arguments 2>$null
        if ($LASTEXITCODE -eq 0) {
            return ($result | Out-String).Trim()
        }
    } catch {
    }

    return ""
}

function New-DefaultReleaseName {
    param([string]$CommitShort)

    $timestamp = Get-Date -Format "yyyy-MM-dd_HHmmss"
    if (-not [string]::IsNullOrWhiteSpace($CommitShort)) {
        return "${timestamp}_${CommitShort}"
    }

    return $timestamp
}

function Test-AppDropLayout {
    param([string]$Root)

    Require-Path -PathValue (Join-Path $Root "bin/redgui.exe") -Label "redgui.exe" | Out-Null
    Assert-MissingPath -PathValue (Join-Path $Root "redgui.exe") -Label "Legacy root redgui.exe"
    Require-Path -PathValue (Join-Path $Root "install_crimson.ps1") -Label "install_crimson.ps1" | Out-Null
    Require-Path -PathValue (Join-Path $Root "install_crimson.cmd") -Label "install_crimson.cmd" | Out-Null
    Require-Path -PathValue (Join-Path $Root "check_crimson_runtime.ps1") -Label "check_crimson_runtime.ps1" | Out-Null
    Require-Path -PathValue (Join-Path $Root "check_crimson_runtime.cmd") -Label "check_crimson_runtime.cmd" | Out-Null
    Require-Path -PathValue (Join-Path $Root "set_crimson_cuda_device.ps1") -Label "set_crimson_cuda_device.ps1" | Out-Null
    Require-Path -PathValue (Join-Path $Root "set_crimson_cuda_device.cmd") -Label "set_crimson_cuda_device.cmd" | Out-Null
    Require-Path -PathValue (Join-Path $Root "README.txt") -Label "README.txt" | Out-Null
    Require-Path -PathValue (Join-Path $Root "share/crimson/fonts") -Label "Fonts directory" | Out-Null
    Require-Path -PathValue (Join-Path $Root "share/crimson/config") -Label "Config directory" | Out-Null
}

$StageRoot = [System.IO.Path]::GetFullPath($StageRoot)
$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)

Require-Path -PathValue $StageRoot -Label "Stage root" | Out-Null
New-Item -ItemType Directory -Force -Path $ShareRoot | Out-Null
Test-AppDropLayout -Root $StageRoot

$publishTimestampUtc = Get-UtcTimestampString
$repoCommit = Try-GetGitString -Arguments @("rev-parse", "HEAD")
$repoCommitShort = Try-GetGitString -Arguments @("rev-parse", "--short", "HEAD")
$repoBranch = Try-GetGitString -Arguments @("rev-parse", "--abbrev-ref", "HEAD")
$latestManifestPath = Join-Path $ShareRoot "latest.json"
$currentRoot = Join-Path $ShareRoot $CurrentName

if (-not [string]::IsNullOrWhiteSpace($ReleaseName) -or $PublishCurrent -or $ArchiveExistingCurrent) {
    if ([string]::IsNullOrWhiteSpace($ReleaseName)) {
        $ReleaseName = New-DefaultReleaseName -CommitShort $repoCommitShort
    }

    $releasesRoot = Join-Path $ShareRoot $ReleasesDirName
    $releaseRoot = Join-Path $releasesRoot $ReleaseName
    New-Item -ItemType Directory -Force -Path $releasesRoot | Out-Null

    if (Test-Path -LiteralPath $releaseRoot) {
        throw "Release root already exists: $releaseRoot"
    }

    if ($ArchiveExistingCurrent -and (Test-Path -LiteralPath $currentRoot)) {
        $archivedCurrentRoot = Join-Path $releasesRoot "current-before-$ReleaseName"
        if (Test-Path -LiteralPath $archivedCurrentRoot) {
            throw "Archived current target already exists: $archivedCurrentRoot"
        }
        Copy-AppTree -SourceRoot $currentRoot -TargetRoot $archivedCurrentRoot -Label "archived current app drop"
    }

    Copy-AppTree -SourceRoot $StageRoot -TargetRoot $releaseRoot -Label "versioned Crimson app release"

    $releaseMetadata = [ordered]@{
        schema_version = 1
        release_name = $ReleaseName
        release_kind = "versioned"
        published_at_utc = $publishTimestampUtc
        commit = $repoCommit
        commit_short = $repoCommitShort
        branch = $repoBranch
        published_target_root = $releaseRoot
    }
    Write-JsonFile -PathValue (Join-Path $releaseRoot "release.json") -Data $releaseMetadata
    Test-AppDropLayout -Root $releaseRoot

    if ($PublishCurrent) {
        Copy-AppTree -SourceRoot $releaseRoot -TargetRoot $currentRoot -Label "current Crimson app drop" -CleanTarget
        Test-AppDropLayout -Root $currentRoot
    }

    $latestMetadata = [ordered]@{
        schema_version = 1
        release_name = $ReleaseName
        published_at_utc = $publishTimestampUtc
        commit = $repoCommit
        commit_short = $repoCommitShort
        branch = $repoBranch
        current_root = $(if ($PublishCurrent) { $currentRoot } else { "" })
        release_root = $releaseRoot
    }
    Write-JsonFile -PathValue $latestManifestPath -Data $latestMetadata

    Write-Host ""
    Write-Host "Published Crimson Windows versioned release:"
    Write-Host "  $releaseRoot"
    if ($PublishCurrent) {
        Write-Host "Refreshed current app drop:"
        Write-Host "  $currentRoot"
    }
} else {
    $effectiveReleaseName = if ([string]::IsNullOrWhiteSpace($ReleaseName)) {
        New-DefaultReleaseName -CommitShort $repoCommitShort
    } else {
        $ReleaseName
    }
    $targetRoot = if ([string]::IsNullOrWhiteSpace($DropName)) {
        $ShareRoot
    } else {
        Join-Path $ShareRoot $DropName
    }

    Copy-AppTree -SourceRoot $StageRoot -TargetRoot $targetRoot -Label "Crimson app drop" -CleanTarget:$CleanDestination

    $releaseMetadata = [ordered]@{
        schema_version = 1
        release_name = $effectiveReleaseName
        release_kind = "direct"
        published_at_utc = $publishTimestampUtc
        commit = $repoCommit
        commit_short = $repoCommitShort
        branch = $repoBranch
        published_target_root = $targetRoot
    }
    Write-JsonFile -PathValue (Join-Path $targetRoot "release.json") -Data $releaseMetadata
    Test-AppDropLayout -Root $targetRoot

    $latestMetadata = [ordered]@{
        schema_version = 1
        release_name = $effectiveReleaseName
        published_at_utc = $publishTimestampUtc
        commit = $repoCommit
        commit_short = $repoCommitShort
        branch = $repoBranch
        current_root = $(if ($DropName -eq $CurrentName) { $targetRoot } else { "" })
        release_root = $targetRoot
    }
    Write-JsonFile -PathValue $latestManifestPath -Data $latestMetadata

    Write-Host ""
    Write-Host "Published Crimson Windows app drop:"
    Write-Host "  $targetRoot"
}

Write-Host ""
Write-Host "Published latest metadata:"
Write-Host "  $latestManifestPath"

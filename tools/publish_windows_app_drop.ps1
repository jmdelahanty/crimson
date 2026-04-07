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

function Require-ExistingCandidate {
    param(
        [string]$Label,
        [string[]]$Candidates
    )

    foreach ($candidate in $Candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    throw "$Label not found. Checked: $($Candidates -join ', ')"
}

function Copy-AppTree {
    param(
        [string]$SourceRoot,
        [string]$TargetRoot,
        [string]$Label,
        [switch]$CleanTarget
    )

    if ($CleanTarget -and (Test-Path -LiteralPath $TargetRoot)) {
        Write-Host "Removing existing $Label:"
        Write-Host "  $TargetRoot"
        Remove-Item -LiteralPath $TargetRoot -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path $TargetRoot | Out-Null
    Write-Host "Copying $Label:"
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
    [System.IO.File]::WriteAllText($PathValue, $json + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
}

function Try-GetGitString {
    param(
        [string[]]$Arguments
    )

    try {
        $result = & git -C $RepoRoot @Arguments 2>$null
        if ($LASTEXITCODE -eq 0) {
            return ($result | Out-String).Trim()
        }
    } catch {
    }

    return ""
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

Require-Path -PathValue $StageRoot -Label "Stage root"
New-Item -ItemType Directory -Force -Path $ShareRoot | Out-Null

$stageExe = Require-Path -PathValue (Join-Path $StageRoot "bin/redgui.exe") -Label "Staged redgui.exe"
Assert-MissingPath -PathValue (Join-Path $StageRoot "redgui.exe") -Label "Legacy staged redgui.exe"
$stageInstallScript = Require-Path -PathValue (Join-Path $StageRoot "install_crimson.ps1") -Label "Staged install_crimson.ps1"
$stageInstallWrapper = Require-Path -PathValue (Join-Path $StageRoot "install_crimson.cmd") -Label "Staged install_crimson.cmd"
$stageReadme = Require-Path -PathValue (Join-Path $StageRoot "README.txt") -Label "Staged README.txt"

$fontsDir = Require-Path -PathValue (Join-Path $StageRoot "share/crimson/fonts") -Label "Fonts directory"
$configDir = Require-Path -PathValue (Join-Path $StageRoot "share/crimson/config") -Label "Config directory"

$publishTimestampUtc = Get-UtcTimestampString
$repoCommit = Try-GetGitString -Arguments @("rev-parse", "HEAD")
$repoCommitShort = Try-GetGitString -Arguments @("rev-parse", "--short", "HEAD")
$repoBranch = Try-GetGitString -Arguments @("rev-parse", "--abbrev-ref", "HEAD")
$latestManifestPath = Join-Path $ShareRoot "latest.json"
$currentRoot = Join-Path $ShareRoot $CurrentName

if (-not [string]::IsNullOrWhiteSpace($ReleaseName) -or $PublishCurrent -or $ArchiveExistingCurrent) {
    if ([string]::IsNullOrWhiteSpace($ReleaseName)) {
        $ReleaseName = Get-Date -Format "yyyy-MM-dd_HHmmss"
    }

    $releasesRoot = Join-Path $ShareRoot $ReleasesDirName
    $releaseRoot = Join-Path $releasesRoot $ReleaseName

    New-Item -ItemType Directory -Force -Path $releasesRoot | Out-Null

    if (Test-Path -LiteralPath $releaseRoot) {
        throw "Release root already exists: $releaseRoot"
    }

    if ($ArchiveExistingCurrent -and (Test-Path -LiteralPath $currentRoot)) {
        $archivedCurrentName = "current-before-$ReleaseName"
        $archivedCurrentRoot = Join-Path $releasesRoot $archivedCurrentName
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

    $publishedExe = Require-Path -PathValue (Join-Path $releaseRoot "bin/redgui.exe") -Label "Published release redgui.exe"
    Assert-MissingPath -PathValue (Join-Path $releaseRoot "redgui.exe") -Label "Legacy published release redgui.exe"
    $publishedInstallScript = Require-Path -PathValue (Join-Path $releaseRoot "install_crimson.ps1") -Label "Published release install_crimson.ps1"
    $publishedInstallWrapper = Require-Path -PathValue (Join-Path $releaseRoot "install_crimson.cmd") -Label "Published release install_crimson.cmd"
    $publishedReadme = Require-Path -PathValue (Join-Path $releaseRoot "README.txt") -Label "Published release README.txt"
    $publishedReleaseMetadata = Require-Path -PathValue (Join-Path $releaseRoot "release.json") -Label "Published release release.json"

    Write-Host ""
    Write-Host "Published Crimson Windows versioned release:"
    Write-Host "  $releaseRoot"
    Write-Host ""
    Write-Host "Verified release:"
    Write-Host "  app:    $publishedExe"
    Write-Host "  install script: $publishedInstallScript"
    Write-Host "  install wrapper: $publishedInstallWrapper"
    Write-Host "  readme: $publishedReadme"
    Write-Host "  release metadata: $publishedReleaseMetadata"
    Write-Host "  fonts:  $(Join-Path $releaseRoot 'share/crimson/fonts')"
    Write-Host "  config: $(Join-Path $releaseRoot 'share/crimson/config')"

    if ($PublishCurrent) {
        Copy-AppTree -SourceRoot $releaseRoot -TargetRoot $currentRoot -Label "current Crimson app drop" -CleanTarget

        $currentExe = Require-Path -PathValue (Join-Path $currentRoot "bin/redgui.exe") -Label "Published current redgui.exe"
        Assert-MissingPath -PathValue (Join-Path $currentRoot "redgui.exe") -Label "Legacy published current redgui.exe"
        $currentInstallScript = Require-Path -PathValue (Join-Path $currentRoot "install_crimson.ps1") -Label "Published current install_crimson.ps1"
        $currentInstallWrapper = Require-Path -PathValue (Join-Path $currentRoot "install_crimson.cmd") -Label "Published current install_crimson.cmd"
        $currentReadme = Require-Path -PathValue (Join-Path $currentRoot "README.txt") -Label "Published current README.txt"
        $currentReleaseMetadata = Require-Path -PathValue (Join-Path $currentRoot "release.json") -Label "Published current release.json"

        Write-Host ""
        Write-Host "Refreshed current Crimson Windows app drop:"
        Write-Host "  $currentRoot"
        Write-Host ""
        Write-Host "Verified current:"
        Write-Host "  app:    $currentExe"
        Write-Host "  install script: $currentInstallScript"
        Write-Host "  install wrapper: $currentInstallWrapper"
        Write-Host "  readme: $currentReadme"
        Write-Host "  release metadata: $currentReleaseMetadata"
        Write-Host "  fonts:  $(Join-Path $currentRoot 'share/crimson/fonts')"
        Write-Host "  config: $(Join-Path $currentRoot 'share/crimson/config')"
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
    $publishedLatestMetadata = Require-Path -PathValue $latestManifestPath -Label "Published latest.json"

    Write-Host ""
    Write-Host "Published latest metadata:"
    Write-Host "  $publishedLatestMetadata"
} else {
    $effectiveReleaseName = if ([string]::IsNullOrWhiteSpace($ReleaseName)) {
        Get-Date -Format "yyyy-MM-dd_HHmmss"
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

    $publishedExe = Require-Path -PathValue (Join-Path $targetRoot "bin/redgui.exe") -Label "Published redgui.exe"
    Assert-MissingPath -PathValue (Join-Path $targetRoot "redgui.exe") -Label "Legacy published redgui.exe"
    $publishedInstallScript = Require-Path -PathValue (Join-Path $targetRoot "install_crimson.ps1") -Label "Published install_crimson.ps1"
    $publishedInstallWrapper = Require-Path -PathValue (Join-Path $targetRoot "install_crimson.cmd") -Label "Published install_crimson.cmd"
    $publishedReadme = Require-Path -PathValue (Join-Path $targetRoot "README.txt") -Label "Published README.txt"
    $publishedReleaseMetadata = Require-Path -PathValue (Join-Path $targetRoot "release.json") -Label "Published release.json"

    Write-Host ""
    Write-Host "Published Crimson Windows app drop:"
    Write-Host "  $targetRoot"
    Write-Host ""
    Write-Host "Verified:"
    Write-Host "  app:    $publishedExe"
    Write-Host "  install script: $publishedInstallScript"
    Write-Host "  install wrapper: $publishedInstallWrapper"
    Write-Host "  readme: $publishedReadme"
    Write-Host "  release metadata: $publishedReleaseMetadata"
    Write-Host "  fonts:  $(Join-Path $targetRoot 'share/crimson/fonts')"
    Write-Host "  config: $(Join-Path $targetRoot 'share/crimson/config')"

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
    $publishedLatestMetadata = Require-Path -PathValue $latestManifestPath -Label "Published latest.json"

    Write-Host ""
    Write-Host "Published latest metadata:"
    Write-Host "  $publishedLatestMetadata"
}

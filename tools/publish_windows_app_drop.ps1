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

if (-not [string]::IsNullOrWhiteSpace($ReleaseName) -or $PublishCurrent -or $ArchiveExistingCurrent) {
    if ([string]::IsNullOrWhiteSpace($ReleaseName)) {
        $ReleaseName = Get-Date -Format "yyyy-MM-dd_HHmmss"
    }

    $releasesRoot = Join-Path $ShareRoot $ReleasesDirName
    $releaseRoot = Join-Path $releasesRoot $ReleaseName
    $currentRoot = Join-Path $ShareRoot $CurrentName

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

    $publishedExe = Require-Path -PathValue (Join-Path $releaseRoot "bin/redgui.exe") -Label "Published release redgui.exe"
    Assert-MissingPath -PathValue (Join-Path $releaseRoot "redgui.exe") -Label "Legacy published release redgui.exe"
    $publishedInstallScript = Require-Path -PathValue (Join-Path $releaseRoot "install_crimson.ps1") -Label "Published release install_crimson.ps1"
    $publishedInstallWrapper = Require-Path -PathValue (Join-Path $releaseRoot "install_crimson.cmd") -Label "Published release install_crimson.cmd"
    $publishedReadme = Require-Path -PathValue (Join-Path $releaseRoot "README.txt") -Label "Published release README.txt"

    Write-Host ""
    Write-Host "Published Crimson Windows versioned release:"
    Write-Host "  $releaseRoot"
    Write-Host ""
    Write-Host "Verified release:"
    Write-Host "  app:    $publishedExe"
    Write-Host "  install script: $publishedInstallScript"
    Write-Host "  install wrapper: $publishedInstallWrapper"
    Write-Host "  readme: $publishedReadme"
    Write-Host "  fonts:  $(Join-Path $releaseRoot 'share/crimson/fonts')"
    Write-Host "  config: $(Join-Path $releaseRoot 'share/crimson/config')"

    if ($PublishCurrent) {
        Copy-AppTree -SourceRoot $releaseRoot -TargetRoot $currentRoot -Label "current Crimson app drop" -CleanTarget

        $currentExe = Require-Path -PathValue (Join-Path $currentRoot "bin/redgui.exe") -Label "Published current redgui.exe"
        Assert-MissingPath -PathValue (Join-Path $currentRoot "redgui.exe") -Label "Legacy published current redgui.exe"
        $currentInstallScript = Require-Path -PathValue (Join-Path $currentRoot "install_crimson.ps1") -Label "Published current install_crimson.ps1"
        $currentInstallWrapper = Require-Path -PathValue (Join-Path $currentRoot "install_crimson.cmd") -Label "Published current install_crimson.cmd"
        $currentReadme = Require-Path -PathValue (Join-Path $currentRoot "README.txt") -Label "Published current README.txt"

        Write-Host ""
        Write-Host "Refreshed current Crimson Windows app drop:"
        Write-Host "  $currentRoot"
        Write-Host ""
        Write-Host "Verified current:"
        Write-Host "  app:    $currentExe"
        Write-Host "  install script: $currentInstallScript"
        Write-Host "  install wrapper: $currentInstallWrapper"
        Write-Host "  readme: $currentReadme"
        Write-Host "  fonts:  $(Join-Path $currentRoot 'share/crimson/fonts')"
        Write-Host "  config: $(Join-Path $currentRoot 'share/crimson/config')"
    }
} else {
    $targetRoot = if ([string]::IsNullOrWhiteSpace($DropName)) {
        $ShareRoot
    } else {
        Join-Path $ShareRoot $DropName
    }

    Copy-AppTree -SourceRoot $StageRoot -TargetRoot $targetRoot -Label "Crimson app drop" -CleanTarget:$CleanDestination

    $publishedExe = Require-Path -PathValue (Join-Path $targetRoot "bin/redgui.exe") -Label "Published redgui.exe"
    Assert-MissingPath -PathValue (Join-Path $targetRoot "redgui.exe") -Label "Legacy published redgui.exe"
    $publishedInstallScript = Require-Path -PathValue (Join-Path $targetRoot "install_crimson.ps1") -Label "Published install_crimson.ps1"
    $publishedInstallWrapper = Require-Path -PathValue (Join-Path $targetRoot "install_crimson.cmd") -Label "Published install_crimson.cmd"
    $publishedReadme = Require-Path -PathValue (Join-Path $targetRoot "README.txt") -Label "Published README.txt"

    Write-Host ""
    Write-Host "Published Crimson Windows app drop:"
    Write-Host "  $targetRoot"
    Write-Host ""
    Write-Host "Verified:"
    Write-Host "  app:    $publishedExe"
    Write-Host "  install script: $publishedInstallScript"
    Write-Host "  install wrapper: $publishedInstallWrapper"
    Write-Host "  readme: $publishedReadme"
    Write-Host "  fonts:  $(Join-Path $targetRoot 'share/crimson/fonts')"
    Write-Host "  config: $(Join-Path $targetRoot 'share/crimson/config')"
}

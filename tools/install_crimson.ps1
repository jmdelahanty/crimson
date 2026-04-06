[CmdletBinding()]
param(
    [string]$SourceRoot,
    [string]$InstallRoot,
    [switch]$ReplaceExisting,
    [switch]$CreateDesktopShortcut,
    [switch]$Launch
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    $SourceRoot = $PSScriptRoot
}

if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        throw "LOCALAPPDATA is not set. Pass -InstallRoot explicitly."
    }
    $InstallRoot = Join-Path $env:LOCALAPPDATA "Crimson"
}

$resolvedSourceRoot = [System.IO.Path]::GetFullPath($SourceRoot).TrimEnd('\', '/')
$resolvedInstallRoot = [System.IO.Path]::GetFullPath($InstallRoot).TrimEnd('\', '/')

if ($resolvedSourceRoot -ieq $resolvedInstallRoot) {
    throw "SourceRoot and InstallRoot are the same: $resolvedSourceRoot`nRun this installer from the published app drop, not from an existing local install."
}

$sourcePrefix = $resolvedSourceRoot + [System.IO.Path]::DirectorySeparatorChar
if ($resolvedInstallRoot.StartsWith($sourcePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "InstallRoot must not be inside SourceRoot.`nSourceRoot:  $resolvedSourceRoot`nInstallRoot: $resolvedInstallRoot"
}

$SourceRoot = $resolvedSourceRoot
$InstallRoot = $resolvedInstallRoot

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

function Read-JsonFile {
    param(
        [string]$PathValue
    )

    if (-not (Test-Path -LiteralPath $PathValue)) {
        return $null
    }

    try {
        return Get-Content -LiteralPath $PathValue -Raw | ConvertFrom-Json
    } catch {
        return $null
    }
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

function Get-UtcTimestampString {
    return (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
}

function Get-SourceUpdateInfo {
    param(
        [string]$ResolvedSourceRoot
    )

    $leaf = Split-Path -Leaf $ResolvedSourceRoot
    $parent = Split-Path -Parent $ResolvedSourceRoot

    if ($leaf -ieq "current") {
        $shareRoot = $parent
        return [ordered]@{
            share_root = $shareRoot
            latest_manifest_path = Join-Path $shareRoot "latest.json"
            current_root = Join-Path $shareRoot "current"
        }
    }

    if ((Split-Path -Leaf $parent) -ieq "releases") {
        $shareRoot = Split-Path -Parent $parent
        return [ordered]@{
            share_root = $shareRoot
            latest_manifest_path = Join-Path $shareRoot "latest.json"
            current_root = Join-Path $shareRoot "current"
        }
    }

    return $null
}

$sourceExe = Require-Path -PathValue (Join-Path $SourceRoot "bin/redgui.exe") -Label "Source redgui.exe"
$sourceFonts = Require-Path -PathValue (Join-Path $SourceRoot "share/crimson/fonts") -Label "Source fonts directory"
$sourceConfig = Require-Path -PathValue (Join-Path $SourceRoot "share/crimson/config") -Label "Source config directory"
$sourceReleaseMetadataPath = Join-Path $SourceRoot "release.json"
$sourceReleaseMetadata = Read-JsonFile -PathValue $sourceReleaseMetadataPath
$sourceUpdateInfo = Get-SourceUpdateInfo -ResolvedSourceRoot $SourceRoot

if (Test-Path -LiteralPath $InstallRoot) {
    if ($ReplaceExisting) {
        Write-Host "Removing existing Crimson install:"
        Write-Host "  $InstallRoot"
        Remove-Item -LiteralPath $InstallRoot -Recurse -Force
    } else {
        throw "Install root already exists: $InstallRoot`nRerun with -ReplaceExisting to update it."
    }
}

$installParent = Split-Path -Parent $InstallRoot
if (-not [string]::IsNullOrWhiteSpace($installParent)) {
    New-Item -ItemType Directory -Force -Path $installParent | Out-Null
}

Write-Host "Installing Crimson app drop:"
Write-Host "  from: $SourceRoot"
Write-Host "  to:   $InstallRoot"

New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null
Copy-Item -Path (Join-Path $SourceRoot "*") -Destination $InstallRoot -Recurse -Force

$installedExe = Require-Path -PathValue (Join-Path $InstallRoot "bin/redgui.exe") -Label "Installed redgui.exe"
$installedFonts = Require-Path -PathValue (Join-Path $InstallRoot "share/crimson/fonts") -Label "Installed fonts directory"
$installedConfig = Require-Path -PathValue (Join-Path $InstallRoot "share/crimson/config") -Label "Installed config directory"
$installedReleaseMetadata = if (Test-Path -LiteralPath (Join-Path $InstallRoot "release.json")) {
    Join-Path $InstallRoot "release.json"
} else {
    ""
}

$installedReleaseName = ""
$installedReleasePublishedUtc = ""
$installedCommit = ""
if ($sourceReleaseMetadata) {
    if ($sourceReleaseMetadata.PSObject.Properties["release_name"]) {
        $installedReleaseName = [string]$sourceReleaseMetadata.release_name
    }
    if ($sourceReleaseMetadata.PSObject.Properties["published_at_utc"]) {
        $installedReleasePublishedUtc = [string]$sourceReleaseMetadata.published_at_utc
    }
    if ($sourceReleaseMetadata.PSObject.Properties["commit"]) {
        $installedCommit = [string]$sourceReleaseMetadata.commit
    }
}

$installMetadata = [ordered]@{
    schema_version = 1
    installed_at_utc = Get-UtcTimestampString
    install_root = $InstallRoot
    source_root = $SourceRoot
    installed_release_name = $installedReleaseName
    installed_release_published_at_utc = $installedReleasePublishedUtc
    installed_commit = $installedCommit
    installed_release_metadata_path = $installedReleaseMetadata
    latest_manifest_path = $(if ($sourceUpdateInfo) { $sourceUpdateInfo.latest_manifest_path } else { "" })
    current_root = $(if ($sourceUpdateInfo) { $sourceUpdateInfo.current_root } else { "" })
    share_root = $(if ($sourceUpdateInfo) { $sourceUpdateInfo.share_root } else { "" })
}
$installMetadataPath = Join-Path $InstallRoot "install_metadata.json"
Write-JsonFile -PathValue $installMetadataPath -Data $installMetadata

if ($CreateDesktopShortcut) {
    $desktopDir = [Environment]::GetFolderPath("Desktop")
    if ([string]::IsNullOrWhiteSpace($desktopDir)) {
        throw "Could not resolve the Desktop folder for this user."
    }

    $shortcutPath = Join-Path $desktopDir "Crimson.lnk"
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = $installedExe
    $shortcut.WorkingDirectory = Split-Path -Parent $installedExe
    $shortcut.IconLocation = $installedExe
    $shortcut.Save()
}

Write-Host ""
Write-Host "Crimson install completed."
Write-Host "Installed:"
Write-Host "  app:    $installedExe"
Write-Host "  fonts:  $installedFonts"
Write-Host "  config: $installedConfig"
Write-Host "  install metadata: $installMetadataPath"

if ($CreateDesktopShortcut) {
    Write-Host "  shortcut: $([System.IO.Path]::Combine([Environment]::GetFolderPath('Desktop'), 'Crimson.lnk'))"
}

if ($Launch) {
    Write-Host ""
    Write-Host "Launching Crimson..."
    Start-Process -FilePath $installedExe -WorkingDirectory (Split-Path -Parent $installedExe)
} else {
    Write-Host ""
    Write-Host "Launch command:"
    Write-Host "  $installedExe"
}

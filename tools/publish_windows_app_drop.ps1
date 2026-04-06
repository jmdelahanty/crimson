[CmdletBinding()]
param(
    [string]$RepoRoot,
    [string]$StageRoot,
    [string]$ShareRoot = "\\YOUR-SERVER\crimson\windows-app",
    [string]$DropName = "current",
    [switch]$CleanDestination
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

$targetRoot = if ([string]::IsNullOrWhiteSpace($DropName)) {
    $ShareRoot
} else {
    Join-Path $ShareRoot $DropName
}

if ($CleanDestination -and (Test-Path -LiteralPath $targetRoot)) {
    Write-Host "Removing existing app drop: $targetRoot"
    Remove-Item -LiteralPath $targetRoot -Recurse -Force
}

$stageExe = Require-Path -PathValue (Join-Path $StageRoot "bin/redgui.exe") -Label "Staged redgui.exe"
Assert-MissingPath -PathValue (Join-Path $StageRoot "redgui.exe") -Label "Legacy staged redgui.exe"
$stageInstallScript = Require-Path -PathValue (Join-Path $StageRoot "install_crimson.ps1") -Label "Staged install_crimson.ps1"
$stageInstallWrapper = Require-Path -PathValue (Join-Path $StageRoot "install_crimson.cmd") -Label "Staged install_crimson.cmd"

$fontsDir = Require-Path -PathValue (Join-Path $StageRoot "share/crimson/fonts") -Label "Fonts directory"
$configDir = Require-Path -PathValue (Join-Path $StageRoot "share/crimson/config") -Label "Config directory"

New-Item -ItemType Directory -Force -Path $targetRoot | Out-Null
Write-Host "Copying staged Crimson app tree:"
Write-Host "  from: $StageRoot"
Write-Host "  to:   $targetRoot"
Copy-Item -Path (Join-Path $StageRoot "*") -Destination $targetRoot -Recurse -Force

$publishedExe = Require-Path -PathValue (Join-Path $targetRoot "bin/redgui.exe") -Label "Published redgui.exe"
Assert-MissingPath -PathValue (Join-Path $targetRoot "redgui.exe") -Label "Legacy published redgui.exe"
$publishedInstallScript = Require-Path -PathValue (Join-Path $targetRoot "install_crimson.ps1") -Label "Published install_crimson.ps1"
$publishedInstallWrapper = Require-Path -PathValue (Join-Path $targetRoot "install_crimson.cmd") -Label "Published install_crimson.cmd"

Write-Host ""
Write-Host "Published Crimson Windows app drop:"
Write-Host "  $targetRoot"
Write-Host ""
Write-Host "Verified:"
Write-Host "  app:    $publishedExe"
Write-Host "  install script: $publishedInstallScript"
Write-Host "  install wrapper: $publishedInstallWrapper"
Write-Host "  fonts:  $(Join-Path $targetRoot 'share/crimson/fonts')"
Write-Host "  config: $(Join-Path $targetRoot 'share/crimson/config')"

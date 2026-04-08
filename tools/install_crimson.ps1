[CmdletBinding()]
param(
    [string]$SourceRoot,
    [string]$InstallRoot,
    [switch]$ReplaceExisting,
    [switch]$CreateDesktopShortcut,
    [switch]$Launch,
    [switch]$SkipPreflightCheck,
    [switch]$SkipPostInstallCheck,
    [switch]$RequireNvidiaSmi,
    [switch]$SkipCudaDevicePrompt
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

function Find-NvidiaSmiPath {
    $command = Get-Command "nvidia-smi" -ErrorAction SilentlyContinue
    if ($command -and -not [string]::IsNullOrWhiteSpace($command.Source)) {
        return $command.Source
    }

    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($env:SystemRoot)) {
        $candidates += (Join-Path $env:SystemRoot "System32/nvidia-smi.exe")
    }
    if (-not [string]::IsNullOrWhiteSpace($env:ProgramFiles)) {
        $candidates += (Join-Path $env:ProgramFiles "NVIDIA Corporation/NVSMI/nvidia-smi.exe")
    }
    if (-not [string]::IsNullOrWhiteSpace(${env:ProgramFiles(x86)})) {
        $candidates += (Join-Path ${env:ProgramFiles(x86)} "NVIDIA Corporation/NVSMI/nvidia-smi.exe")
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    return $null
}

function Get-CudaDeviceConfigPath {
    if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        return $null
    }
    return Join-Path $env:LOCALAPPDATA "Crimson\config\cuda_device.json"
}

function Get-NvidiaSmiDevices {
    param(
        [string]$NvidiaSmiPath
    )

    if ([string]::IsNullOrWhiteSpace($NvidiaSmiPath)) {
        return @()
    }

    $lines = & $NvidiaSmiPath `
        "--query-gpu=index,name,memory.total,driver_version" `
        "--format=csv,noheader,nounits" 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $lines) {
        return @()
    }

    $devices = @()
    foreach ($line in $lines) {
        $text = $line.ToString().Trim()
        if ([string]::IsNullOrWhiteSpace($text)) {
            continue
        }
        $parts = $text.Split(",")
        if ($parts.Count -lt 4) {
            continue
        }

        $index = 0
        $memoryMb = 0
        if (-not [int]::TryParse($parts[0].Trim(), [ref]$index)) {
            continue
        }
        [void][int]::TryParse($parts[2].Trim(), [ref]$memoryMb)

        $devices += [PSCustomObject]@{
            index = $index
            name = $parts[1].Trim()
            memory_mb = $memoryMb
            driver_version = $parts[3].Trim()
        }
    }

    return @($devices | Sort-Object index)
}

function Write-CudaDevicePreference {
    param(
        [string]$ConfigPath,
        [object]$Device
    )

    if ([string]::IsNullOrWhiteSpace($ConfigPath) -or $null -eq $Device) {
        return
    }

    $payload = [ordered]@{
        schema_version = 1
        selected_cuda_device_index = [int]$Device.index
        selected_cuda_device_name = [string]$Device.name
        selected_cuda_device_memory_bytes = ([int64]$Device.memory_mb * 1MB)
        selected_cuda_device_memory_mb = [int]$Device.memory_mb
        driver_version = [string]$Device.driver_version
        saved_at_utc = Get-UtcTimestampString
        saved_by = "install_crimson.ps1"
    }

    Write-JsonFile -PathValue $ConfigPath -Data $payload
}

function MaybeConfigureCudaDevicePreference {
    if ($SkipCudaDevicePrompt) {
        return
    }

    $configPath = Get-CudaDeviceConfigPath
    if ([string]::IsNullOrWhiteSpace($configPath)) {
        return
    }

    $nvidiaSmiPath = Find-NvidiaSmiPath
    if ([string]::IsNullOrWhiteSpace($nvidiaSmiPath)) {
        Write-Host ""
        Write-Host "Skipping CUDA GPU selection:"
        Write-Host "  nvidia-smi.exe was not found."
        return
    }

    $devices = Get-NvidiaSmiDevices -NvidiaSmiPath $nvidiaSmiPath
    if ($devices.Count -le 1) {
        return
    }

    $existingPreference = Read-JsonFile -PathValue $configPath
    if ($existingPreference -and
        $existingPreference.PSObject.Properties["selected_cuda_device_index"]) {
        $existingIndex = [int]$existingPreference.selected_cuda_device_index
        $existingDevice = $devices | Where-Object { $_.index -eq $existingIndex } |
            Select-Object -First 1
        if ($existingDevice) {
            Write-Host ""
            Write-Host "Existing saved CUDA GPU preference detected:"
            Write-Host ("  GPU {0} | {1} | {2} MiB" -f `
                $existingDevice.index, $existingDevice.name, $existingDevice.memory_mb)
            Write-Host "Keeping that preference. Delete the saved config or rerun with a different choice later if needed."
            return
        }
    }

    $recommendedDevice = $devices | Sort-Object memory_mb, index -Descending |
        Select-Object -First 1

    Write-Host ""
    Write-Host "Multiple NVIDIA GPUs detected."
    Write-Host "Select the CUDA GPU Crimson should prefer for video decode and rendering."
    Write-Host "If the selected GPU does not match the display/OpenGL GPU, Crimson may ask again on first launch."
    Write-Host ""
    foreach ($device in $devices) {
        $recommendedSuffix = if ($recommendedDevice -and $device.index -eq $recommendedDevice.index) {
            " [Suggested stronger GPU]"
        } else {
            ""
        }
        Write-Host ("  GPU {0} | {1} | {2} MiB | driver {3}{4}" -f `
            $device.index, $device.name, $device.memory_mb, $device.driver_version, $recommendedSuffix)
    }
    Write-Host ""

    while ($true) {
        $prompt = "Enter CUDA GPU index to remember for Crimson, or press Enter to skip and let Crimson ask on first launch"
        $response = Read-Host $prompt
        if ([string]::IsNullOrWhiteSpace($response)) {
            Write-Host "Skipping saved CUDA GPU preference."
            return
        }

        $parsedIndex = 0
        if (-not [int]::TryParse($response.Trim(), [ref]$parsedIndex)) {
            Write-Host "Invalid GPU index: $response"
            continue
        }

        $selectedDevice = $devices | Where-Object { $_.index -eq $parsedIndex } |
            Select-Object -First 1
        if (-not $selectedDevice) {
            Write-Host "No detected NVIDIA GPU uses index $parsedIndex."
            continue
        }

        Write-CudaDevicePreference -ConfigPath $configPath -Device $selectedDevice
        Write-Host ""
        Write-Host "Saved Crimson CUDA GPU preference:"
        Write-Host ("  GPU {0} | {1}" -f $selectedDevice.index, $selectedDevice.name)
        Write-Host "  config: $configPath"
        return
    }
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

function Invoke-RuntimeCheck {
    param(
        [string]$AppRoot,
        [string]$Label
    )

    $runtimeCheckScript = Join-Path $AppRoot "check_crimson_runtime.ps1"
    if (-not (Test-Path -LiteralPath $runtimeCheckScript)) {
        Write-Host ""
        Write-Host "Skipping Crimson runtime check ($Label):"
        Write-Host "  missing: $runtimeCheckScript"
        return
    }

    Write-Host ""
    Write-Host "Running Crimson runtime check ($Label)..."
    if ($RequireNvidiaSmi) {
        & $runtimeCheckScript -AppRoot $AppRoot -RequireNvidiaSmi
    } else {
        & $runtimeCheckScript -AppRoot $AppRoot
    }
}

$sourceExe = Require-Path -PathValue (Join-Path $SourceRoot "bin/redgui.exe") -Label "Source redgui.exe"
$sourceFonts = Require-Path -PathValue (Join-Path $SourceRoot "share/crimson/fonts") -Label "Source fonts directory"
$sourceConfig = Require-Path -PathValue (Join-Path $SourceRoot "share/crimson/config") -Label "Source config directory"
$sourceReleaseMetadataPath = Join-Path $SourceRoot "release.json"
$sourceReleaseMetadata = Read-JsonFile -PathValue $sourceReleaseMetadataPath
$sourceUpdateInfo = Get-SourceUpdateInfo -ResolvedSourceRoot $SourceRoot

if (-not $SkipPreflightCheck) {
    Invoke-RuntimeCheck -AppRoot $SourceRoot -Label "source app drop"
}

MaybeConfigureCudaDevicePreference

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

if (-not $SkipPostInstallCheck) {
    Invoke-RuntimeCheck -AppRoot $InstallRoot -Label "installed app"
}

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

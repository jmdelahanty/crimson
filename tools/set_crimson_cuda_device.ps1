[CmdletBinding()]
param(
    [string]$AppRoot,
    [int]$DeviceIndex = -1,
    [switch]$ClearSavedChoice
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($AppRoot)) {
    $AppRoot = $PSScriptRoot
}

$AppRoot = [System.IO.Path]::GetFullPath($AppRoot).TrimEnd('\', '/')

function Read-JsonFile {
    param([string]$PathValue)
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
    [System.IO.File]::WriteAllText(
        $PathValue,
        $json + [Environment]::NewLine,
        [System.Text.UTF8Encoding]::new($false))
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
    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        return Join-Path $env:LOCALAPPDATA "Crimson\config\cuda_device.json"
    }
    return Join-Path $AppRoot "cuda_device.json"
}

function Get-NvidiaSmiDevices {
    param([string]$NvidiaSmiPath)

    if ([string]::IsNullOrWhiteSpace($NvidiaSmiPath)) {
        return @()
    }

    $lines = & $NvidiaSmiPath "--query-gpu=index,name,memory.total,driver_version" "--format=csv,noheader,nounits" 2>$null
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
        [object]$SelectedDevice
    )

    $payload = [ordered]@{
        schema_version = 1
        selected_cuda_device_index = [int]$SelectedDevice.index
        selected_cuda_device_name = [string]$SelectedDevice.name
        selected_cuda_device_memory_bytes = ([int64]$SelectedDevice.memory_mb * 1MB)
        selected_cuda_device_memory_mb = [int]$SelectedDevice.memory_mb
        driver_version = [string]$SelectedDevice.driver_version
        saved_at_utc = Get-UtcTimestampString
        saved_by = "set_crimson_cuda_device.ps1"
    }

    Write-JsonFile -PathValue $ConfigPath -Data $payload
}

function Describe-Device {
    param([object]$Device)
    return ("GPU {0} | {1} | {2} MiB | driver {3}" -f $Device.index, $Device.name, $Device.memory_mb, $Device.driver_version)
}

$configPath = Get-CudaDeviceConfigPath
$existingPreference = Read-JsonFile -PathValue $configPath

Write-Host "Crimson CUDA GPU preference tool"
Write-Host "  app root: $AppRoot"
Write-Host "  config:   $configPath"

if ($ClearSavedChoice) {
    if (Test-Path -LiteralPath $configPath) {
        Remove-Item -LiteralPath $configPath -Force
        Write-Host ""
        Write-Host "Removed saved CUDA GPU preference."
    } else {
        Write-Host ""
        Write-Host "No saved CUDA GPU preference was present."
    }
    return
}

$nvidiaSmiPath = Find-NvidiaSmiPath
if ([string]::IsNullOrWhiteSpace($nvidiaSmiPath)) {
    throw "nvidia-smi.exe was not found. Install an NVIDIA driver first."
}

$devices = Get-NvidiaSmiDevices -NvidiaSmiPath $nvidiaSmiPath
if ($devices.Count -eq 0) {
    throw "No NVIDIA GPUs were detected by nvidia-smi."
}

Write-Host ""
Write-Host "Detected NVIDIA GPUs:"
foreach ($device in $devices) {
    Write-Host ("  {0}" -f (Describe-Device -Device $device))
}

if ($existingPreference -and $existingPreference.PSObject.Properties["selected_cuda_device_index"]) {
    $savedIndex = [int]$existingPreference.selected_cuda_device_index
    $savedDevice = $devices | Where-Object { $_.index -eq $savedIndex } | Select-Object -First 1
    Write-Host ""
    Write-Host "Current saved preference:"
    if ($savedDevice) {
        Write-Host ("  {0}" -f (Describe-Device -Device $savedDevice))
    } else {
        Write-Host ("  GPU {0} | no longer present in current nvidia-smi output" -f $savedIndex)
    }
}

$recommendedDevice = $devices | Sort-Object memory_mb, index -Descending | Select-Object -First 1
Write-Host ""
Write-Host ("Suggested GPU: {0}" -f (Describe-Device -Device $recommendedDevice))

$selectedDevice = $null
if ($DeviceIndex -ge 0) {
    $selectedDevice = $devices | Where-Object { $_.index -eq $DeviceIndex } | Select-Object -First 1
    if (-not $selectedDevice) {
        throw "Requested GPU index $DeviceIndex was not found."
    }
} elseif ($devices.Count -eq 1) {
    $selectedDevice = $devices[0]
} else {
    Write-Host ""
    Write-Host "Enter the CUDA GPU index Crimson should prefer."
    Write-Host "Press Enter to keep the existing saved choice and exit."

    while ($true) {
        $response = Read-Host "CUDA GPU index"
        if ([string]::IsNullOrWhiteSpace($response)) {
            Write-Host "Leaving the saved preference unchanged."
            return
        }

        $parsedIndex = 0
        if (-not [int]::TryParse($response.Trim(), [ref]$parsedIndex)) {
            Write-Host "Invalid GPU index: $response"
            continue
        }

        $selectedDevice = $devices | Where-Object { $_.index -eq $parsedIndex } | Select-Object -First 1
        if (-not $selectedDevice) {
            Write-Host "No detected NVIDIA GPU uses index $parsedIndex."
            continue
        }
        break
    }
}

Write-CudaDevicePreference -ConfigPath $configPath -SelectedDevice $selectedDevice

Write-Host ""
Write-Host "Saved Crimson CUDA GPU preference:"
Write-Host ("  {0}" -f (Describe-Device -Device $selectedDevice))

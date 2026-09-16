[CmdletBinding()]
param(
    [string]$AppRoot,
    [switch]$RequireNvidiaSmi
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($AppRoot)) {
    $AppRoot = $PSScriptRoot
}

$AppRoot = [System.IO.Path]::GetFullPath($AppRoot).TrimEnd('\', '/')
$results = New-Object System.Collections.Generic.List[object]
$hasFailures = $false

function Add-Result {
    param(
        [string]$Category,
        [string]$Name,
        [string]$Status,
        [string]$Details
    )

    if ($Status -eq "FAIL") {
        $script:hasFailures = $true
    }

    [void]$results.Add([PSCustomObject]@{
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

$appRootExists = Test-RequiredPath -Category "app" -Name "App root" -PathValue $AppRoot
$binDir = Join-Path $AppRoot "bin"
$shareRoot = Join-Path $AppRoot "share/crimson"
$exePath = Join-Path $binDir "redgui.exe"
$fontsDir = Join-Path $shareRoot "fonts"
$configDir = Join-Path $shareRoot "config"
$releaseMetadataPath = Join-Path $AppRoot "release.json"
$installMetadataPath = Join-Path $AppRoot "install_metadata.json"
$installScriptPath = Join-Path $AppRoot "install_crimson.ps1"
$runtimeCheckScriptPath = Join-Path $AppRoot "check_crimson_runtime.ps1"
$cudaDeviceScriptPath = Join-Path $AppRoot "set_crimson_cuda_device.ps1"

if ($appRootExists) {
    Test-RequiredPath -Category "app" -Name "Crimson executable" -PathValue $exePath | Out-Null
    Test-RequiredPath -Category "app" -Name "Fonts directory" -PathValue $fontsDir | Out-Null
    Test-RequiredPath -Category "app" -Name "Config directory" -PathValue $configDir | Out-Null

    if (Test-Path -LiteralPath $binDir) {
        $dllCount = @(Get-ChildItem -LiteralPath $binDir -Filter *.dll -ErrorAction SilentlyContinue).Count
        if ($dllCount -gt 0) {
            Add-Ok -Category "app" -Name "Bundled runtime DLLs" -Details "$dllCount files in $binDir"
        } else {
            Add-Warn -Category "app" -Name "Bundled runtime DLLs" -Details "No .dll files were found in $binDir"
        }
    }

    foreach ($scriptCheck in @(
        @("Installer script", $installScriptPath),
        @("Runtime check script", $runtimeCheckScriptPath),
        @("CUDA device script", $cudaDeviceScriptPath)
    )) {
        if (Test-Path -LiteralPath $scriptCheck[1]) {
            Add-Ok -Category "app" -Name $scriptCheck[0] -Details $scriptCheck[1]
        } else {
            Add-Warn -Category "app" -Name $scriptCheck[0] -Details "missing: $($scriptCheck[1])"
        }
    }
}

$longPathPolicyPath = "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem"
try {
    $longPathsEnabled = Get-ItemPropertyValue `
        -Path $longPathPolicyPath `
        -Name "LongPathsEnabled" `
        -ErrorAction Stop
    if ([int]$longPathsEnabled -eq 1) {
        Add-Ok -Category "system" -Name "Win32 long paths" `
            -Details "LongPathsEnabled=1"
    } else {
        Add-Warn -Category "system" -Name "Win32 long paths" `
            -Details "LongPathsEnabled=$longPathsEnabled; deep Zarr paths may be reported as missing"
    }
} catch {
    Add-Warn -Category "system" -Name "Win32 long paths" `
        -Details "Could not read $longPathPolicyPath\LongPathsEnabled"
}

$releaseMetadata = Read-JsonFile -PathValue $releaseMetadataPath
if ($releaseMetadata) {
    $releaseName = if ($releaseMetadata.PSObject.Properties["release_name"]) { [string]$releaseMetadata.release_name } else { "<unknown>" }
    $commitShort = if ($releaseMetadata.PSObject.Properties["commit_short"]) { [string]$releaseMetadata.commit_short } else { "" }
    $publishedUtc = if ($releaseMetadata.PSObject.Properties["published_at_utc"]) { [string]$releaseMetadata.published_at_utc } else { "" }
    $details = $releaseName
    if (-not [string]::IsNullOrWhiteSpace($commitShort)) { $details += " | commit $commitShort" }
    if (-not [string]::IsNullOrWhiteSpace($publishedUtc)) { $details += " | published $publishedUtc" }
    Add-Ok -Category "metadata" -Name "Release metadata" -Details $details
} else {
    Add-Warn -Category "metadata" -Name "Release metadata" -Details "missing or unreadable: $releaseMetadataPath"
}

$installMetadata = Read-JsonFile -PathValue $installMetadataPath
if ($installMetadata) {
    $installedAt = if ($installMetadata.PSObject.Properties["installed_at_utc"]) { [string]$installMetadata.installed_at_utc } else { "" }
    $installedRelease = if ($installMetadata.PSObject.Properties["installed_release_name"]) { [string]$installMetadata.installed_release_name } else { "" }
    $details = if (-not [string]::IsNullOrWhiteSpace($installedRelease)) { $installedRelease } else { "present" }
    if (-not [string]::IsNullOrWhiteSpace($installedAt)) { $details += " | installed $installedAt" }
    Add-Ok -Category "metadata" -Name "Install metadata" -Details $details
} else {
    Add-Warn -Category "metadata" -Name "Install metadata" -Details "missing or unreadable: $installMetadataPath"
}

if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
    Add-Warn -Category "runtime" -Name "Crash dump directory" -Details "LOCALAPPDATA is not set"
} else {
    $crashDumpDir = Join-Path $env:LOCALAPPDATA "Crimson/CrashDumps"
    if (Test-Path -LiteralPath $crashDumpDir) {
        $dumpCount = @(Get-ChildItem -LiteralPath $crashDumpDir -Filter *.dmp -ErrorAction SilentlyContinue).Count
        Add-Ok -Category "runtime" -Name "Crash dump directory" -Details "$crashDumpDir | dumps: $dumpCount"
    } else {
        Add-Warn -Category "runtime" -Name "Crash dump directory" -Details "not created yet: $crashDumpDir"
    }
}

$nvidiaSmiPath = Find-NvidiaSmiPath
if ($nvidiaSmiPath) {
    try {
        $gpuSummary = & $nvidiaSmiPath "--query-gpu=name,driver_version" "--format=csv,noheader" 2>&1
        if ($LASTEXITCODE -eq 0 -and $gpuSummary) {
            $detail = (($gpuSummary | ForEach-Object { $_.ToString().Trim() }) -join "; ")
            Add-Ok -Category "nvidia" -Name "nvidia-smi" -Details "$nvidiaSmiPath | $detail"
        } else {
            $statusDetails = "$nvidiaSmiPath | query failed"
            if ($RequireNvidiaSmi) { Add-Fail -Category "nvidia" -Name "nvidia-smi" -Details $statusDetails } else { Add-Warn -Category "nvidia" -Name "nvidia-smi" -Details $statusDetails }
        }
    } catch {
        $statusDetails = "$nvidiaSmiPath | query failed: $($_.Exception.Message)"
        if ($RequireNvidiaSmi) { Add-Fail -Category "nvidia" -Name "nvidia-smi" -Details $statusDetails } else { Add-Warn -Category "nvidia" -Name "nvidia-smi" -Details $statusDetails }
    }
} else {
    $statusDetails = "nvidia-smi.exe not found in PATH or standard NVIDIA locations"
    if ($RequireNvidiaSmi) { Add-Fail -Category "nvidia" -Name "nvidia-smi" -Details $statusDetails } else { Add-Warn -Category "nvidia" -Name "nvidia-smi" -Details $statusDetails }
}

Write-Host ""
Write-Host "Crimson runtime check:"
Write-Host "  App root: $AppRoot"
Write-Host ""
$results | Format-Table -AutoSize | Out-String | Write-Host

$okCount = @($results | Where-Object { $_.Status -eq "OK" }).Count
$warnCount = @($results | Where-Object { $_.Status -eq "WARN" }).Count
$failCount = @($results | Where-Object { $_.Status -eq "FAIL" }).Count

Write-Host "Summary: OK=$okCount WARN=$warnCount FAIL=$failCount"

if ($hasFailures) {
    throw "Crimson runtime check failed."
}

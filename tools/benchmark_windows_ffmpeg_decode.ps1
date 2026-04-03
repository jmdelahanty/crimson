[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputVideo,

    [string]$FfmpegRoot = $(if ($env:CRIMSON_FFMPEG_ROOT) {
        $env:CRIMSON_FFMPEG_ROOT
    } else {
        "C:/third_party/ffmpeg-nvidia"
    }),

    [switch]$IncludeSoftwareNative,
    [switch]$IncludeCudaDownloadNv12
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

function Get-StreamMetadata {
    param(
        [string]$FfprobeExe,
        [string]$VideoPath
    )

    $probeArgs = @(
        "-v", "error",
        "-select_streams", "v:0",
        "-show_entries", "stream=codec_name,codec_long_name,profile,width,height,avg_frame_rate,r_frame_rate,nb_frames",
        "-of", "json",
        $VideoPath
    )
    $probeOutput = & $FfprobeExe @probeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "ffprobe failed for $VideoPath"
    }

    $probeJson = $probeOutput | ConvertFrom-Json
    if (-not $probeJson.streams -or $probeJson.streams.Count -lt 1) {
        throw "ffprobe did not return a primary video stream for $VideoPath"
    }

    return $probeJson.streams[0]
}

function Convert-FractionToDouble {
    param(
        [string]$Fraction
    )

    if ([string]::IsNullOrWhiteSpace($Fraction)) {
        return $null
    }

    if ($Fraction -notmatch "^\d+/\d+$") {
        return $null
    }

    $parts = $Fraction.Split("/")
    $numerator = [double]$parts[0]
    $denominator = [double]$parts[1]
    if ($denominator -eq 0.0) {
        return $null
    }

    return ($numerator / $denominator)
}

function Invoke-DecodeBenchmark {
    param(
        [string]$Name,
        [string]$FfmpegExe,
        [string[]]$FfmpegArgs
    )

    Write-Host ""
    Write-Host "=== $Name ==="
    $cleanArgs = @($FfmpegArgs | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($cleanArgs.Count -eq 0) {
        throw "No FFmpeg arguments were provided for benchmark mode '$Name'."
    }
    $quotedArgs = $cleanArgs | ForEach-Object {
        if ($_ -match '[\s"]') {
            '"' + ($_ -replace '"', '\"') + '"'
        } else {
            $_
        }
    }
    $argumentLine = $quotedArgs -join " "
    Write-Host ("ffmpeg " + $argumentLine)

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $stdoutFile = [System.IO.Path]::GetTempFileName()
    $stderrFile = [System.IO.Path]::GetTempFileName()
    try {
        $process = Start-Process -FilePath $FfmpegExe `
                                 -ArgumentList $argumentLine `
                                 -NoNewWindow `
                                 -Wait `
                                 -PassThru `
                                 -RedirectStandardOutput $stdoutFile `
                                 -RedirectStandardError $stderrFile
        $exitCode = $process.ExitCode
        $stopwatch.Stop()

        $stderrText = if (Test-Path -LiteralPath $stderrFile) {
            Get-Content -LiteralPath $stderrFile -Raw
        } else {
            ""
        }
        $stdoutText = if (Test-Path -LiteralPath $stdoutFile) {
            Get-Content -LiteralPath $stdoutFile -Raw
        } else {
            ""
        }
        $outputText = $stderrText + $stdoutText
    } finally {
        Remove-Item -LiteralPath $stdoutFile -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $stderrFile -Force -ErrorAction SilentlyContinue
    }

    $frameMatches = [regex]::Matches($outputText, "frame=\s*(\d+)")
    $frameCount = $null
    if ($frameMatches.Count -gt 0) {
        $frameCount = [int]$frameMatches[$frameMatches.Count - 1].Groups[1].Value
    }

    $speedMatches = [regex]::Matches($outputText, "speed=\s*([0-9\.]+)x")
    $reportedSpeed = $null
    if ($speedMatches.Count -gt 0) {
        $reportedSpeed = [double]$speedMatches[$speedMatches.Count - 1].Groups[1].Value
    }

    if ($exitCode -ne 0) {
        Write-Host $outputText
        return [pscustomobject]@{
            Mode          = $Name
            Status        = "FAILED"
            Seconds       = [math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
            Frames        = $frameCount
            DecodedFps    = $null
            ReportedSpeed = $reportedSpeed
        }
    }

    $decodedFps = $null
    if ($frameCount -and $stopwatch.Elapsed.TotalSeconds -gt 0.0) {
        $decodedFps = [math]::Round(($frameCount / $stopwatch.Elapsed.TotalSeconds), 2)
    }

    return [pscustomobject]@{
        Mode          = $Name
        Status        = "OK"
        Seconds       = [math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
        Frames        = $frameCount
        DecodedFps    = $decodedFps
        ReportedSpeed = $reportedSpeed
    }
}

$ffmpegExe = Join-Path $FfmpegRoot "bin\ffmpeg.exe"
$ffprobeExe = Join-Path $FfmpegRoot "bin\ffprobe.exe"

Require-Path $InputVideo "Input video"
Require-Path $FfmpegRoot "FFmpeg root"
Require-Path $ffmpegExe "ffmpeg.exe"
Require-Path $ffprobeExe "ffprobe.exe"

$stream = Get-StreamMetadata -FfprobeExe $ffprobeExe -VideoPath $InputVideo
$avgFps = Convert-FractionToDouble -Fraction $stream.avg_frame_rate
$nominalFps = Convert-FractionToDouble -Fraction $stream.r_frame_rate

Write-Host "Input stream:"
Write-Host "  path:    $InputVideo"
Write-Host "  codec:   $($stream.codec_name) ($($stream.profile))"
Write-Host "  size:    $($stream.width)x$($stream.height)"
if ($avgFps) {
    Write-Host "  avg fps: $([math]::Round($avgFps, 3))"
}
if ($nominalFps) {
    Write-Host "  r fps:   $([math]::Round($nominalFps, 3))"
}
if ($stream.nb_frames) {
    Write-Host "  frames:  $($stream.nb_frames)"
}

$softwareBaseArgs = @(
    "-hide_banner",
    "-loglevel", "info",
    "-stats",
    "-threads", "0",
    "-vsync", "0",
    "-an",
    "-sn",
    "-dn",
    "-i", $InputVideo
)

$cudaBaseArgs = @(
    "-hide_banner",
    "-loglevel", "info",
    "-stats",
    "-threads", "0",
    "-vsync", "0",
    "-hwaccel", "cuda",
    "-hwaccel_output_format", "cuda",
    "-an",
    "-sn",
    "-dn",
    "-i", $InputVideo
)

$benchmarks = New-Object System.Collections.Generic.List[hashtable]

if ($IncludeSoftwareNative) {
    $benchmarks.Add(@{
        Name = "software-native"
        Args = $softwareBaseArgs + @(
            "-f", "null", "NUL"
        )
    })
}

$benchmarks.Add(@{
    Name = "software-rgba"
    Args = $softwareBaseArgs + @(
        "-vf", "format=rgba",
        "-f", "null", "NUL"
    )
})

if ($IncludeCudaDownloadNv12) {
    $benchmarks.Add(@{
        Name = "cuda-download-nv12"
        Args = $cudaBaseArgs + @(
            "-vf", "hwdownload,format=nv12",
            "-f", "null", "NUL"
        )
    })
}

$benchmarks.Add(@{
    Name = "cuda-download-rgba"
    Args = $cudaBaseArgs + @(
        "-vf", "hwdownload,format=nv12,format=rgba",
        "-f", "null", "NUL"
    )
})

$results = foreach ($benchmark in $benchmarks) {
    Invoke-DecodeBenchmark -Name $benchmark.Name -FfmpegExe $ffmpegExe -FfmpegArgs $benchmark.Args
}

Write-Host ""
Write-Host "Summary:"
$results | Format-Table -AutoSize

$softwareRgba = $results | Where-Object { $_.Mode -eq "software-rgba" -and $_.Status -eq "OK" } | Select-Object -First 1
$cudaRgba = $results | Where-Object { $_.Mode -eq "cuda-download-rgba" -and $_.Status -eq "OK" } | Select-Object -First 1

if ($softwareRgba -and $cudaRgba -and $softwareRgba.DecodedFps -and $cudaRgba.DecodedFps) {
    Write-Host ""
    if ($cudaRgba.DecodedFps -gt $softwareRgba.DecodedFps) {
        $ratio = [math]::Round(($cudaRgba.DecodedFps / $softwareRgba.DecodedFps), 2)
        Write-Host "CUDA download+RGBA outperformed software RGBA by ${ratio}x."
    } elseif ($softwareRgba.DecodedFps -gt $cudaRgba.DecodedFps) {
        $ratio = [math]::Round(($softwareRgba.DecodedFps / $cudaRgba.DecodedFps), 2)
        Write-Host "Software RGBA outperformed CUDA download+RGBA by ${ratio}x."
    } else {
        Write-Host "Software RGBA and CUDA download+RGBA performed similarly."
    }
}

Write-Host ""
Write-Host "Interpretation:"
Write-Host "  software-rgba approximates a software stimulus decode path."
Write-Host "  cuda-download-rgba approximates NVDEC plus download/conversion overhead."
Write-Host "  If software-rgba wins clearly, a software stimulus decoder is worth trying in Crimson."

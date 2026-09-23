[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Redgui,

    [Parameter(Mandatory = $true)]
    [string]$Zarr,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [Parameter(Mandatory = $true)]
    [ValidateSet(
        "workspace",
        "overlays",
        "stimulus-debug",
        "crop-preview",
        "analysis-eye",
        "analysis-tail-stimulus"
    )]
    [string]$UiState,

    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 2147483647)]
    [int]$Frame,

    [Parameter(Mandatory = $true)]
    [string]$StateName,

    [ValidateRange(1, 2147483647)]
    [int]$Width = 1920,

    [ValidateRange(1, 2147483647)]
    [int]$Height = 1080,

    [ValidateRange(0.1, 3600.0)]
    [double]$TimeoutSeconds = 60.0,

    [ValidateRange(0, 60000)]
    [int]$SettleMilliseconds = 500,

    [string]$ImguiIni = "",

    [string]$SourceRevision = "unknown",

    [ValidateSet("deterministic_exact_read_only")]
    [string]$CaptureClass = "deterministic_exact_read_only",

    [switch]$KeepRunDirectory,

    [switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Assert-ReferenceCondition {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw "UI reference marker validation failed: $Message"
    }
}

function ConvertTo-WindowsCommandLineArgument {
    param([AllowEmptyString()][string]$Value)

    if ($Value.Length -eq 0) {
        return '""'
    }
    if ($Value -notmatch '[\s"]') {
        return $Value
    }

    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.Append([char]34)
    $backslashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq [char]92) {
            $backslashes++
            continue
        }
        if ($character -eq [char]34) {
            if ($backslashes -gt 0) {
                [void]$builder.Append(
                    [string]::new([char]92, (($backslashes * 2) + 1))
                )
            }
            else {
                [void]$builder.Append([char]92)
            }
            [void]$builder.Append([char]34)
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) {
            [void]$builder.Append([string]::new([char]92, $backslashes))
            $backslashes = 0
        }
        [void]$builder.Append($character)
    }
    if ($backslashes -gt 0) {
        [void]$builder.Append([string]::new([char]92, ($backslashes * 2)))
    }
    [void]$builder.Append([char]34)
    return $builder.ToString()
}

function Test-UiReferenceMarker {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Marker,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedState,

        [Parameter(Mandatory = $true)]
        [int]$ExpectedFrame
    )

    Assert-ReferenceCondition ($Marker.format -eq "crimson_ui_reference_v1") "unexpected format"
    Assert-ReferenceCondition ($Marker.state -eq $ExpectedState) "unexpected state"
    Assert-ReferenceCondition ($Marker.write_contract -eq "read-only") "write contract is not read-only"
    Assert-ReferenceCondition ([int]$Marker.target_frame -eq $ExpectedFrame) "target frame mismatch"
    Assert-ReferenceCondition ([int]$Marker.presented_frame -eq $ExpectedFrame) "presented frame mismatch"
    Assert-ReferenceCondition ([int]$Marker.current_frame -eq $ExpectedFrame) "current frame mismatch"
    Assert-ReferenceCondition ([int]$Marker.slider_frame -eq $ExpectedFrame) "slider frame mismatch"
    Assert-ReferenceCondition ([int]$Marker.stable_frames -ge 60) "fewer than 60 stable frames"
    Assert-ReferenceCondition ([int]$Marker.buffers.camera_capacity -gt 0) "camera ring is not configured"
    Assert-ReferenceCondition (
        [int]$Marker.buffers.camera_valid -eq [int]$Marker.buffers.camera_capacity
    ) "camera ring is incomplete"

    if ($ExpectedState -eq "stimulus-debug") {
        Assert-ReferenceCondition ([int]$Marker.buffers.stimulus_capacity -gt 0) "stimulus ring is not configured"
        Assert-ReferenceCondition (
            [int]$Marker.buffers.stimulus_valid -le [int]$Marker.buffers.stimulus_capacity
        ) "stimulus occupancy exceeds capacity"
        Assert-ReferenceCondition ([bool]$Marker.stimulus.loaded) "stimulus video is not loaded"
        Assert-ReferenceCondition ([bool]$Marker.stimulus.debug_windows) "stimulus debug windows are hidden"
        Assert-ReferenceCondition (
            [int]$Marker.stimulus.target_frame -eq [int]$Marker.stimulus.presented_frame
        ) "mapped stimulus texture mismatch"
    }

    if ($ExpectedState -eq "crop-preview") {
        Assert-ReferenceCondition ([bool]$Marker.crop.ready) "crop preview is not ready"
        Assert-ReferenceCondition ([int]$Marker.crop.source_frame -eq $ExpectedFrame) "crop source frame mismatch"
        Assert-ReferenceCondition ([int]$Marker.crop.width -gt 0) "crop width is empty"
        Assert-ReferenceCondition ([int]$Marker.crop.height -gt 0) "crop height is empty"
    }

    if (($ExpectedState -eq "overlays") -or ($ExpectedState -eq "analysis-eye")) {
        Assert-ReferenceCondition (
            $Marker.overlays.optional_overlay_status -eq "optional overlays ready"
        ) "optional overlays are not ready"
        Assert-ReferenceCondition ([int]$Marker.overlays.visible_roi_count -gt 0) "no visible mask ROI"
        Assert-ReferenceCondition ([int]$Marker.overlays.component_fill_count -gt 0) "no component fill"
        Assert-ReferenceCondition ([int]$Marker.overlays.contours_drawn -gt 0) "no mask contour"
        Assert-ReferenceCondition ([int]$Marker.overlays.axes_drawn -gt 0) "no eye axis"
        Assert-ReferenceCondition (
            ([int]$Marker.overlays.gaze_rays_drawn -gt 0) -or
            ([int]$Marker.overlays.visual_cones_drawn -gt 0)
        ) "no eye-direction geometry"
        Assert-ReferenceCondition ([int]$Marker.overlays.angle_labels_drawn -gt 0) "no angle label"
    }

    if ($ExpectedState -eq "analysis-eye") {
        Assert-ReferenceCondition ([bool]$Marker.analysis.state_applied) "eye analysis state was not applied"
        Assert-ReferenceCondition ([bool]$Marker.analysis.show_eye) "eye trace is hidden"
        Assert-ReferenceCondition (-not [bool]$Marker.analysis.show_tail_angle) "tail angle is unexpectedly visible"
        Assert-ReferenceCondition (-not [bool]$Marker.analysis.show_tail_deflection) "tail deflection is unexpectedly visible"
        Assert-ReferenceCondition (-not [bool]$Marker.analysis.show_tail_curvature) "tail curvature is unexpectedly visible"
        Assert-ReferenceCondition (-not [bool]$Marker.analysis.show_stimulus_context) "stimulus context is unexpectedly visible"
    }

    if ($ExpectedState -eq "analysis-tail-stimulus") {
        Assert-ReferenceCondition ([bool]$Marker.analysis.state_applied) "tail analysis state was not applied"
        Assert-ReferenceCondition (-not [bool]$Marker.analysis.show_eye) "eye trace is unexpectedly visible"
        Assert-ReferenceCondition ([bool]$Marker.analysis.show_tail_angle) "tail angle is hidden"
        Assert-ReferenceCondition ([bool]$Marker.analysis.show_tail_deflection) "tail deflection is hidden"
        Assert-ReferenceCondition ([bool]$Marker.analysis.show_tail_curvature) "tail curvature is hidden"
        Assert-ReferenceCondition ([bool]$Marker.analysis.show_stimulus_context) "stimulus context is hidden"
    }

    Assert-ReferenceCondition (
        $Marker.rendered_image.surface -eq "opengl_front_buffer"
    ) "primary image is not the OpenGL front buffer"
    Assert-ReferenceCondition (
        [int]$Marker.rendered_image.width -eq [int]$Marker.framebuffer_size.width
    ) "rendered image width mismatch"
    Assert-ReferenceCondition (
        [int]$Marker.rendered_image.height -eq [int]$Marker.framebuffer_size.height
    ) "rendered image height mismatch"
}

if (-not (Test-Path -LiteralPath $Redgui -PathType Leaf)) {
    throw "redgui executable not found: $Redgui"
}
if (-not (Test-Path -LiteralPath $Zarr -PathType Container)) {
    throw "Zarr archive not found: $Zarr"
}
if (($ImguiIni.Length -gt 0) -and
    (-not (Test-Path -LiteralPath $ImguiIni -PathType Leaf))) {
    throw "ImGui profile not found: $ImguiIni"
}

$Redgui = (Resolve-Path -LiteralPath $Redgui).Path
$Zarr = (Resolve-Path -LiteralPath $Zarr).Path
if ($ImguiIni.Length -gt 0) {
    $ImguiIni = (Resolve-Path -LiteralPath $ImguiIni).Path
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path

if (-not ("CrimsonReferenceNative" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public sealed class CrimsonWindowBounds
{
    public int X { get; set; }
    public int Y { get; set; }
    public int Width { get; set; }
    public int Height { get; set; }
}

public static class CrimsonReferenceNative
{
    [StructLayout(LayoutKind.Sequential)]
    private struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT
    {
        public int X;
        public int Y;
    }

    private const int GWL_STYLE = -16;
    private const int GWL_EXSTYLE = -20;
    private const uint SWP_NOZORDER = 0x0004;
    private const uint SWP_NOACTIVATE = 0x0010;
    private const int SW_RESTORE = 9;

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetClientRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongW", SetLastError = true)]
    private static extern int GetWindowLong32(IntPtr hWnd, int index);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW", SetLastError = true)]
    private static extern IntPtr GetWindowLongPtr64(IntPtr hWnd, int index);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool AdjustWindowRectExForDpi(
        ref RECT rect, uint style, bool menu, uint exStyle, uint dpi);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool SetWindowPos(
        IntPtr hWnd, IntPtr insertAfter, int x, int y, int width, int height,
        uint flags);

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hWnd, int command);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassNameW(
        IntPtr hWnd, StringBuilder className, int maxCount);

    private static long GetWindowLongValue(IntPtr hWnd, int index)
    {
        return IntPtr.Size == 8
            ? GetWindowLongPtr64(hWnd, index).ToInt64()
            : GetWindowLong32(hWnd, index);
    }

    private static void ThrowLastError(string operation)
    {
        throw new Win32Exception(
            Marshal.GetLastWin32Error(), operation + " failed");
    }

    public static uint Dpi(IntPtr hWnd)
    {
        try
        {
            uint dpi = GetDpiForWindow(hWnd);
            return dpi == 0 ? 96u : dpi;
        }
        catch (EntryPointNotFoundException)
        {
            return 96u;
        }
    }

    public static void ResizeClient(IntPtr hWnd, int width, int height)
    {
        RECT outer = new RECT { Left = 0, Top = 0, Right = width, Bottom = height };
        bool adjusted = false;
        try
        {
            adjusted = AdjustWindowRectExForDpi(
                ref outer,
                unchecked((uint)GetWindowLongValue(hWnd, GWL_STYLE)),
                false,
                unchecked((uint)GetWindowLongValue(hWnd, GWL_EXSTYLE)),
                Dpi(hWnd));
        }
        catch (EntryPointNotFoundException)
        {
            RECT currentClient = new RECT();
            RECT currentWindow = new RECT();
            if (!GetClientRect(hWnd, out currentClient) ||
                !GetWindowRect(hWnd, out currentWindow))
            {
                ThrowLastError("GetClientRect/GetWindowRect");
            }
            POINT clientOrigin = new POINT { X = 0, Y = 0 };
            if (!ClientToScreen(hWnd, ref clientOrigin))
            {
                ThrowLastError("ClientToScreen");
            }
            int nonClientWidth =
                (currentWindow.Right - currentWindow.Left) -
                (currentClient.Right - currentClient.Left);
            int nonClientHeight =
                (currentWindow.Bottom - currentWindow.Top) -
                (currentClient.Bottom - currentClient.Top);
            outer.Left = currentWindow.Left - clientOrigin.X;
            outer.Top = currentWindow.Top - clientOrigin.Y;
            outer.Right = outer.Left + width + nonClientWidth;
            outer.Bottom = outer.Top + height + nonClientHeight;
            adjusted = true;
        }
        if (!adjusted)
        {
            ThrowLastError("AdjustWindowRectExForDpi");
        }
        if (!SetWindowPos(
                hWnd,
                IntPtr.Zero,
                outer.Left,
                outer.Top,
                outer.Right - outer.Left,
                outer.Bottom - outer.Top,
                SWP_NOZORDER | SWP_NOACTIVATE))
        {
            ThrowLastError("SetWindowPos");
        }
    }

    public static CrimsonWindowBounds ClientBounds(IntPtr hWnd)
    {
        RECT rect;
        if (!GetClientRect(hWnd, out rect))
        {
            ThrowLastError("GetClientRect");
        }
        POINT origin = new POINT { X = rect.Left, Y = rect.Top };
        if (!ClientToScreen(hWnd, ref origin))
        {
            ThrowLastError("ClientToScreen");
        }
        return new CrimsonWindowBounds {
            X = origin.X,
            Y = origin.Y,
            Width = rect.Right - rect.Left,
            Height = rect.Bottom - rect.Top
        };
    }

    public static CrimsonWindowBounds WindowBounds(IntPtr hWnd)
    {
        RECT rect;
        if (!GetWindowRect(hWnd, out rect))
        {
            ThrowLastError("GetWindowRect");
        }
        return new CrimsonWindowBounds {
            X = rect.Left,
            Y = rect.Top,
            Width = rect.Right - rect.Left,
            Height = rect.Bottom - rect.Top
        };
    }

    public static string WindowClass(IntPtr hWnd)
    {
        StringBuilder value = new StringBuilder(256);
        int length = GetClassNameW(hWnd, value, value.Capacity);
        return length == 0 ? "" : value.ToString();
    }

    public static void BringToFront(IntPtr hWnd)
    {
        ShowWindow(hWnd, SW_RESTORE);
        SetForegroundWindow(hWnd);
    }
}
'@
}

Add-Type -AssemblyName System.Drawing

if ($ValidateOnly) {
    [ordered]@{
        status = "validated"
        redgui = $Redgui
        zarr = $Zarr
        output_directory = $OutputDirectory
        ui_state = $UiState
        frame = $Frame
        state_name = $StateName
        client_size = [ordered]@{ width = $Width; height = $Height }
        imgui_ini = $(if ($ImguiIni.Length -gt 0) { $ImguiIni } else { $null })
        powershell = $PSVersionTable.PSVersion.ToString()
        note = "PowerShell parameters and embedded Win32 helper compiled; no GUI was launched."
    } | ConvertTo-Json -Depth 4
    return
}

$runDirectory = Join-Path $env:TEMP (
    "crimson-phase5l-reference-" + [guid]::NewGuid().ToString("N")
)
New-Item -ItemType Directory -Path $runDirectory | Out-Null
$readyPath = Join-Path $runDirectory "ui-reference-ready.json"
$stdoutPath = Join-Path $runDirectory "redgui.stdout.log"
$stderrPath = Join-Path $runDirectory "redgui.stderr.log"
$process = $null
$processStarted = $false
$stdoutTask = $null
$stderrTask = $null
$logsWritten = $false
$captureSucceeded = $false
$prefix = Join-Path $OutputDirectory $StateName

function Stop-ReferenceProcess {
    if ((-not $processStarted) -or ($null -eq $process) -or $process.HasExited) {
        return
    }
    if (-not $process.CloseMainWindow()) {
        $process.Kill()
    }
    elseif (-not $process.WaitForExit(5000)) {
        $process.Kill()
    }
    $process.WaitForExit()
}

function Write-ReferenceLogs {
    if ($logsWritten -or (-not $processStarted) -or ($null -eq $process)) {
        return
    }
    if (-not $process.HasExited) {
        return
    }
    $stdout = if ($null -eq $stdoutTask) { "" } else {
        $stdoutTask.GetAwaiter().GetResult()
    }
    $stderr = if ($null -eq $stderrTask) { "" } else {
        $stderrTask.GetAwaiter().GetResult()
    }
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($stdoutPath, $stdout, $utf8)
    [IO.File]::WriteAllText($stderrPath, $stderr, $utf8)
    [IO.File]::WriteAllText(
        "$prefix.redgui.log",
        "=== stdout ===`r`n$stdout`r`n=== stderr ===`r`n$stderr",
        $utf8
    )
    Copy-Item -LiteralPath $stdoutPath -Destination "$prefix.stdout.log" -Force
    Copy-Item -LiteralPath $stderrPath -Destination "$prefix.stderr.log" -Force
    $script:logsWritten = $true
}

try {
    if ($ImguiIni.Length -gt 0) {
        Copy-Item -LiteralPath $ImguiIni -Destination (Join-Path $runDirectory "imgui.ini")
    }

    $arguments = @(
        "--zarr", $Zarr,
        "--swap-interval", "0",
        "--frame-cap-fps", "60",
        "--no-mask-perf-log",
        "--ui-reference-state", $UiState,
        "--ui-reference-frame", $Frame.ToString([Globalization.CultureInfo]::InvariantCulture),
        "--ui-reference-ready-file", $readyPath,
        "--ui-reference-timeout", $TimeoutSeconds.ToString([Globalization.CultureInfo]::InvariantCulture)
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Redgui
    $startInfo.Arguments = (($arguments | ForEach-Object {
        ConvertTo-WindowsCommandLineArgument ([string]$_)
    }) -join " ")
    $startInfo.WorkingDirectory = $runDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "failed to start redgui"
    }
    $processStarted = $true
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()

    $windowDeadline = [DateTime]::UtcNow.AddSeconds(30)
    $windowHandle = [IntPtr]::Zero
    while ([DateTime]::UtcNow -lt $windowDeadline) {
        if ($process.HasExited) {
            throw "redgui exited before its Win32 window appeared (exit $($process.ExitCode))"
        }
        $process.Refresh()
        $windowHandle = $process.MainWindowHandle
        if ($windowHandle -ne [IntPtr]::Zero) {
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if ($windowHandle -eq [IntPtr]::Zero) {
        throw "timed out waiting for the redgui Win32 window"
    }

    [CrimsonReferenceNative]::ResizeClient($windowHandle, $Width, $Height)
    $sizeDeadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        $clientBounds = [CrimsonReferenceNative]::ClientBounds($windowHandle)
        if (($clientBounds.Width -eq $Width) -and ($clientBounds.Height -eq $Height)) {
            break
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $sizeDeadline)
    Assert-ReferenceCondition (
        ($clientBounds.Width -eq $Width) -and ($clientBounds.Height -eq $Height)
    ) "Win32 client size is $($clientBounds.Width)x$($clientBounds.Height), expected ${Width}x${Height}"

    $readyDeadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds + 10.0)
    while (-not (Test-Path -LiteralPath $readyPath -PathType Leaf)) {
        if ($process.HasExited) {
            throw "redgui exited before the ready marker (exit $($process.ExitCode))"
        }
        if ([DateTime]::UtcNow -ge $readyDeadline) {
            throw "timed out waiting for the UI reference ready marker"
        }
        Start-Sleep -Milliseconds 100
    }

    $marker = Get-Content -LiteralPath $readyPath -Raw | ConvertFrom-Json
    Test-UiReferenceMarker $marker $UiState $Frame
    $renderedImagePath = [string]$marker.rendered_image.path
    if (-not (Test-Path -LiteralPath $renderedImagePath -PathType Leaf)) {
        throw "UI reference rendered image is missing: $renderedImagePath"
    }

    [CrimsonReferenceNative]::BringToFront($windowHandle)
    Start-Sleep -Milliseconds $SettleMilliseconds
    $clientBounds = [CrimsonReferenceNative]::ClientBounds($windowHandle)
    $windowBounds = [CrimsonReferenceNative]::WindowBounds($windowHandle)
    Assert-ReferenceCondition (
        ($clientBounds.Width -eq $Width) -and ($clientBounds.Height -eq $Height)
    ) "Win32 client size changed before capture"
    Assert-ReferenceCondition (
        ($clientBounds.X -eq 0) -and ($clientBounds.Y -eq 0)
    ) "Win32 client origin is $($clientBounds.X),$($clientBounds.Y), expected 0,0"

    $win32Png = "$prefix.win32.png"
    $bitmap = [System.Drawing.Bitmap]::new(
        $clientBounds.Width,
        $clientBounds.Height,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
    )
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen(
                $clientBounds.X,
                $clientBounds.Y,
                0,
                0,
                $bitmap.Size,
                [System.Drawing.CopyPixelOperation]::SourceCopy
            )
        }
        finally {
            $graphics.Dispose()
        }
        $bitmap.Save($win32Png, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $bitmap.Dispose()
    }

    $primaryPng = "$prefix.png"
    Copy-Item -LiteralPath $renderedImagePath -Destination $primaryPng -Force
    Copy-Item -LiteralPath $readyPath -Destination "$prefix.ui-reference.json" -Force

    $executableHash = (Get-FileHash -LiteralPath $Redgui -Algorithm SHA256).Hash.ToLowerInvariant()
    $primaryHash = (Get-FileHash -LiteralPath $primaryPng -Algorithm SHA256).Hash.ToLowerInvariant()
    $win32Hash = (Get-FileHash -LiteralPath $win32Png -Algorithm SHA256).Hash.ToLowerInvariant()
    $gpu = @()
    try {
        $gpu = @(Get-CimInstance Win32_VideoController | ForEach-Object {
            [ordered]@{
                name = $_.Name
                driver_version = $_.DriverVersion
            }
        })
    }
    catch {
        $gpu = @([ordered]@{ name = "unavailable"; driver_version = "unavailable" })
    }

    $metadata = [ordered]@{
        schema = "crimson.phase5l.reference.v1"
        captured_at = [DateTimeOffset]::Now.ToString("o")
        state = $StateName
        capture_class = $CaptureClass
        source_revision = $SourceRevision
        executable = $Redgui
        executable_sha256 = $executableHash
        archive = $Zarr
        archive_mtime = (Get-Item -LiteralPath $Zarr).LastWriteTimeUtc.ToString("o")
        requested_client_size = [ordered]@{ width = $Width; height = $Height }
        win32 = [ordered]@{
            process_id = $process.Id
            hwnd = ("0x{0:X}" -f $windowHandle.ToInt64())
            title = $process.MainWindowTitle
            class = [CrimsonReferenceNative]::WindowClass($windowHandle)
            dpi = [CrimsonReferenceNative]::Dpi($windowHandle)
            client = [ordered]@{
                x = $clientBounds.X
                y = $clientBounds.Y
                width = $clientBounds.Width
                height = $clientBounds.Height
            }
            window = [ordered]@{
                x = $windowBounds.X
                y = $windowBounds.Y
                width = $windowBounds.Width
                height = $windowBounds.Height
            }
            png_sha256 = $win32Hash
        }
        platform = [ordered]@{
            os = [Environment]::OSVersion.VersionString
            powershell = $PSVersionTable.PSVersion.ToString()
            gpu = $gpu
        }
        capture_surface = "application_gl_front_buffer"
        ui_reference = $marker
        png_sha256 = $primaryHash
    }
    $metadata | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath "$prefix.json" -Encoding UTF8

    $checksumLines = @(
        "$primaryHash  $([IO.Path]::GetFileName($primaryPng))",
        "$win32Hash  $([IO.Path]::GetFileName($win32Png))"
    )
    [IO.File]::WriteAllLines(
        "$prefix.sha256.txt",
        $checksumLines,
        ([System.Text.UTF8Encoding]::new($false))
    )

    Stop-ReferenceProcess
    Write-ReferenceLogs
    $capturedIni = Join-Path $runDirectory "imgui.ini"
    if (Test-Path -LiteralPath $capturedIni -PathType Leaf) {
        Copy-Item -LiteralPath $capturedIni -Destination "$prefix.imgui.ini" -Force
    }

    $captureSucceeded = $true
    Write-Host "captured: $primaryPng"
    Write-Host "Win32 companion: $win32Png"
    Write-Host "metadata: $prefix.json"
}
finally {
    if ($processStarted -and ($null -ne $process) -and (-not $process.HasExited)) {
        Stop-ReferenceProcess
    }
    if ($processStarted -and ($null -ne $process) -and $process.HasExited) {
        Write-ReferenceLogs
    }
    if ($null -ne $process) {
        $process.Dispose()
    }
    if ($KeepRunDirectory -or (-not $captureSucceeded)) {
        Write-Warning "preserved capture run directory: $runDirectory"
    }
    else {
        Remove-Item -LiteralPath $runDirectory -Recurse -Force
    }
}

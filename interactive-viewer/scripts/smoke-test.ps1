param([string]$GlbPath, [string]$ShotPath, [int]$WaitSeconds = 5)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Shot {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@

# Per-Monitor V2 DPI awareness so coordinates and captures are physical.
[Win32Shot]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

# Resolve the built viewer relative to this script so the smoke lane is not
# tied to any workstation path.
$exe = Join-Path $PSScriptRoot '..\x64\Release\Preview3D.exe'
$p = Start-Process -FilePath $exe -ArgumentList "`"$GlbPath`"" -PassThru
Start-Sleep -Seconds $WaitSeconds
$p.Refresh()

if ($p.HasExited) {
    Write-Host "FAILED: process exited early with code $($p.ExitCode)"
    exit 1
}

$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) {
    Write-Host 'FAILED: no main window handle'
    Stop-Process -Id $p.Id -Force
    exit 1
}
Write-Host "window handle: $h, title: '$($p.MainWindowTitle)'"

$rect = New-Object Win32Shot+RECT
[Win32Shot]::GetWindowRect($h, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left
$hgt = $rect.Bottom - $rect.Top
Write-Host "window rect (physical): ${w}x${hgt}"
if ($w -le 0 -or $hgt -le 0) {
    Write-Host 'FAILED: bad window rect'
    Stop-Process -Id $p.Id -Force
    exit 1
}

$bmp = New-Object System.Drawing.Bitmap($w, $hgt)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [Win32Shot]::PrintWindow($h, $hdc, 2)  # PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc)
$g.Dispose()
Write-Host "PrintWindow result: $ok"
$bmp.Save($ShotPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "saved screenshot: $ShotPath (${w}x${hgt})"

Stop-Process -Id $p.Id -Force
Write-Host 'process stopped; smoke test OK'

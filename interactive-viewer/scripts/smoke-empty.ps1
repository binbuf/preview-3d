param([string]$ShotPath, [int]$WaitSeconds = 4)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Shot2 {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr value);
}
"@
[Win32Shot2]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

# Resolve the built viewer relative to this script so the smoke lane is not
# tied to any workstation path.
$exe = Join-Path $PSScriptRoot '..\x64\Release\Preview3D.exe'
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds $WaitSeconds
$p.Refresh()
if ($p.HasExited) { Write-Host "FAILED: exited with $($p.ExitCode)"; exit 1 }
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Host 'FAILED: no window'; Stop-Process -Id $p.Id -Force; exit 1 }

$rect = New-Object Win32Shot2+RECT
[Win32Shot2]::GetWindowRect($h, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left
$hgt = $rect.Bottom - $rect.Top

$bmp = New-Object System.Drawing.Bitmap($w, $hgt)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[Win32Shot2]::PrintWindow($h, $hdc, 2) | Out-Null
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save($ShotPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "captured empty-state ${w}x${hgt}: $ShotPath"
Stop-Process -Id $p.Id -Force

# Check for the empty-state card: dashed rounded card fill (0x242426 at 0.92 over bg)
$b = New-Object System.Drawing.Bitmap($ShotPath)
$card = 0
for ($y = 200; $y -lt 1000; $y += 3) {
    for ($x = 300; $x -lt 1200; $x += 3) {
        $c = $b.GetPixel($x, $y)
        if ([Math]::Abs($c.R - 35) -le 3 -and [Math]::Abs($c.G - 35) -le 3 -and [Math]::Abs($c.B - 37) -le 3) { $card++ }
    }
}
$b.Dispose()
Write-Host "empty-card pixel samples: $card (expect thousands)"
if ($card -gt 100) { Write-Host 'VERDICT: empty state card rendered OK'; exit 0 }
Write-Host 'VERDICT: empty card not detected'
exit 2

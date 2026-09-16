param(
    [ValidateSet('Debug','Release')][string]$Configuration='Debug',
    [string]$Result
)
$ErrorActionPreference='Stop'
$root=Resolve-Path (Join-Path $PSScriptRoot '..\..')
$exe=Join-Path $root "x64\$Configuration\Preview3D.exe"
$good=Resolve-Path (Join-Path $root 'interactive-viewer\test-assets\corpus\A-small-glb.glb')
$bad=Resolve-Path (Join-Path $root 'interactive-viewer\test-assets\corpus\empty.ply')
$pressure=Resolve-Path (Join-Path $root 'interactive-viewer\test-assets\corpus\Pressure.glb')
Add-Type -AssemblyName UIAutomationClient
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Preview3DNative {
  [StructLayout(LayoutKind.Sequential)] struct COPYDATASTRUCT { public UIntPtr dwData; public int cbData; public IntPtr lpData; }
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint m,UIntPtr w,IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,UIntPtr w,IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int cx,int cy,uint flags);
  [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h,int index);
  [DllImport("user32.dll")] public static extern IntPtr GetSystemMenu(IntPtr h,bool revert);
  [DllImport("user32.dll")] public static extern IntPtr GetLastActivePopup(IntPtr h);
  public static long HitTest(IntPtr h,int x,int y) { long packed=((long)(ushort)x)|((long)(ushort)y<<16); return SendMessage(h,0x84,UIntPtr.Zero,(IntPtr)packed).ToInt64(); }
  public static IntPtr SendPath(IntPtr h,string path) {
    IntPtr text=Marshal.StringToHGlobalUni(path); IntPtr data=IntPtr.Zero;
    try { var copy=new COPYDATASTRUCT{dwData=(UIntPtr)104,cbData=(path.Length+1)*2,lpData=text};
      data=Marshal.AllocHGlobal(Marshal.SizeOf<COPYDATASTRUCT>()); Marshal.StructureToPtr(copy,data,false);
      return SendMessage(h,0x004A,UIntPtr.Zero,data);
    } finally { if(data!=IntPtr.Zero)Marshal.FreeHGlobal(data); Marshal.FreeHGlobal(text); }
  }
}
'@
function Assert($condition,[string]$message){if(!$condition){throw $message}}
function Start-Viewer([string]$path,[string]$switch='--app-smoke'){
    $process=Start-Process -FilePath $exe -ArgumentList @($switch,$path) -PassThru
    for($i=0;$i -lt 120 -and $process.MainWindowHandle -eq 0;$i++){Start-Sleep -Milliseconds 50;$process.Refresh()}
    Assert ($process.MainWindowHandle -ne 0) 'viewer window did not appear'
    Start-Sleep -Milliseconds 700
    return $process
}

function Children($process){
    $element=[System.Windows.Automation.AutomationElement]::FromHandle($process.MainWindowHandle)
    return $element.FindAll([System.Windows.Automation.TreeScope]::Children,[System.Windows.Automation.Condition]::TrueCondition)
}
function Find-Control($children,[string]$name){
    for($i=0;$i -lt $children.Count;$i++){if($children.Item($i).Current.Name -eq $name){return $children.Item($i)}}
    throw "UIA control '$name' was not found"
}
$checks=[System.Collections.Generic.List[string]]::new()
$loading=Start-Viewer $good '--progressive-smoke'
try {
    for($i=0;$i -lt 80 -and (Children $loading).Count -lt 20;$i++){Start-Sleep -Milliseconds 100}
    Assert ((Children $loading).Count -ge 20) 'initial model did not become accessible'
    Assert ([Preview3DNative]::SendPath($loading.MainWindowHandle,[string]$pressure) -ne [IntPtr]::Zero) 'loading replacement command failed'
    $during=Children $loading
    $grid=Find-Control $during 'Ground grid'
    ([System.Windows.Automation.TogglePattern]$grid.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)).Toggle()
    ([System.Windows.Automation.InvokePattern](Find-Control $during 'Fit selection or model').GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)).Invoke()
    $checks.Add('existing controls remain callable while replacement work is active')
} finally {
    if(!$loading.HasExited){[Preview3DNative]::PostMessage($loading.MainWindowHandle,0x10,[UIntPtr]0,[IntPtr]0)|Out-Null;$loading.WaitForExit(12000)|Out-Null}
    if(!$loading.HasExited){$loading.Kill()}
}
$viewer=Start-Viewer $good
try {
    $controls=Children $viewer
    foreach($expected in @('Ground grid','Axis snap','Travel speed','Fit selection or model','Reset view','Share','More options','Open with','Minimize','Maximize','Close','Model information','Zoom','Fullscreen','View from positive X','View from negative Z')){
        $control=Find-Control $controls $expected
        Assert $control.Current.IsKeyboardFocusable "$expected is not keyboard focusable"
    }
    Assert ((Find-Control $controls 'Ground grid').Current.ControlType -eq [System.Windows.Automation.ControlType]::CheckBox) 'Grid role is not CheckBox'
    Assert ((Find-Control $controls 'Zoom').Current.ControlType -eq [System.Windows.Automation.ControlType]::Slider) 'Zoom role is not Slider'
    $checks.Add('UIA names, roles, states, and keyboard-focusable custom controls')

    $style=[Preview3DNative]::GetWindowLongPtr($viewer.MainWindowHandle,-16).ToInt64()
    Assert (($style -band 0x00080000) -ne 0 -and [Preview3DNative]::GetSystemMenu($viewer.MainWindowHandle,$false) -ne [IntPtr]::Zero) 'Alt+Space system menu contract is missing'
    $maximize=Find-Control $controls 'Maximize';$bounds=$maximize.Current.BoundingRectangle
    Assert ([Preview3DNative]::HitTest($viewer.MainWindowHandle,[int]($bounds.X+$bounds.Width/2),[int]($bounds.Y+$bounds.Height/2)) -eq 9) 'maximize does not expose HTMAXBUTTON for Snap Layouts'
    $fullscreen=Find-Control $controls 'Fullscreen';$fullToggle=[System.Windows.Automation.TogglePattern]$fullscreen.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    $fullToggle.Toggle();Start-Sleep -Milliseconds 100
    Assert (([System.Windows.Automation.TogglePattern]$fullscreen.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)).Current.ToggleState -eq [System.Windows.Automation.ToggleState]::On) 'fullscreen did not remain distinct from maximize'
    ([System.Windows.Automation.TogglePattern]$fullscreen.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)).Toggle()
    $checks.Add('Alt+Space, Snap Layout hit test, and fullscreen/maximize distinction')

    $grid=Find-Control $controls 'Ground grid'
    $grid.SetFocus()
    Assert $grid.Current.HasKeyboardFocus 'UIA SetFocus did not focus Grid'
    $toggle=[System.Windows.Automation.TogglePattern]$grid.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    $before=$toggle.Current.ToggleState; $toggle.Toggle(); Start-Sleep -Milliseconds 100
    $after=([System.Windows.Automation.TogglePattern]$grid.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)).Current.ToggleState
    Assert ($before -ne $after) 'Grid Toggle pattern did not change state'
    ([System.Windows.Automation.InvokePattern](Find-Control $controls 'Fit selection or model').GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)).Invoke()
    ([System.Windows.Automation.InvokePattern](Find-Control $controls 'View from positive X').GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)).Invoke()
    $checks.Add('UIA focus, Toggle, and Invoke patterns')

    [Preview3DNative]::PostMessage($viewer.MainWindowHandle,0x111,[UIntPtr]32771,[IntPtr]0)|Out-Null
    $dialog=[IntPtr]::Zero
    for($i=0;$i -lt 50 -and ($dialog -eq [IntPtr]::Zero -or $dialog -eq $viewer.MainWindowHandle);$i++){
        Start-Sleep -Milliseconds 100;$dialog=[Preview3DNative]::GetLastActivePopup($viewer.MainWindowHandle)
    }
    Assert ($dialog -ne [IntPtr]::Zero -and $dialog -ne $viewer.MainWindowHandle) 'Open dialog did not appear'
    [Preview3DNative]::PostMessage($dialog,0x10,[UIntPtr]0,[IntPtr]0)|Out-Null
    Start-Sleep -Milliseconds 200
    $grid.SetFocus();Assert $grid.Current.HasKeyboardFocus 'focus did not restore after the Open dialog'
    $checks.Add('focus restoration after the native Open dialog')

    [Preview3DNative]::SetWindowPos($viewer.MainWindowHandle,[IntPtr]::Zero,0,0,480,360,0x0006)|Out-Null
    foreach($dpi in @(96,144,192)){
        $ok=[Preview3DNative]::SendMessage($viewer.MainWindowHandle,0x8068,[UIntPtr]67,[IntPtr]$dpi)
        Assert ($ok -eq 1) "DPI seam rejected $dpi"
        Assert ((Children $viewer).Count -ge 16) "controls disappeared at DPI $dpi"
    }
    $checks.Add('narrow layout at 100, 150, and 200 percent DPI layout scales')

    [Preview3DNative]::SendMessage($viewer.MainWindowHandle,0x8068,[UIntPtr]68,[IntPtr]3)|Out-Null
    $flags=[Preview3DNative]::SendMessage($viewer.MainWindowHandle,0x8068,[UIntPtr]69,[IntPtr]0).ToInt64()
    Assert ($flags -eq 3) 'high-contrast/reduced-motion state was not applied'
    [Preview3DNative]::SendMessage($viewer.MainWindowHandle,0x8068,[UIntPtr]68,[IntPtr]0)|Out-Null
    $checks.Add('high contrast and reduced motion application paths')
} finally {
    if(!$viewer.HasExited){[Preview3DNative]::PostMessage($viewer.MainWindowHandle,0x10,[UIntPtr]0,[IntPtr]0)|Out-Null;$viewer.WaitForExit(12000)|Out-Null}
    if(!$viewer.HasExited){$viewer.Kill()}
}

$failure=Start-Viewer $bad
try {
    Start-Sleep -Milliseconds 800
    $controls=Children $failure
    foreach($expected in @('Retry','Open another','Copy details')){
        $control=$null
        for($i=0;$i -lt $controls.Count;$i++){
            $candidate=$controls.Item($i)
            if($candidate.Current.Name -eq $expected -and $candidate.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button){$control=$candidate;break}
        }
        Assert ($null -ne $control) "$expected role is not Button"
        Assert $control.Current.IsKeyboardFocusable "$expected is not keyboard focusable"
    }
    $checks.Add('native error actions exposed as focusable UIA Buttons')
} finally {
    if(!$failure.HasExited){[Preview3DNative]::PostMessage($failure.MainWindowHandle,0x10,[UIntPtr]0,[IntPtr]0)|Out-Null;$failure.WaitForExit(12000)|Out-Null}
    if(!$failure.HasExited){$failure.Kill()}
}
$report=[ordered]@{configuration=$Configuration;status='passed';checks=$checks}
$json=$report|ConvertTo-Json -Depth 3
$json
if($Result){$json|Set-Content -Encoding utf8 $Result}

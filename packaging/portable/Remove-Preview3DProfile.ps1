[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

if (Get-Process -Name Preview3D, Preview3DImportWorker, Preview3DImportHost, Preview3DStepHost -ErrorAction SilentlyContinue) {
    throw 'Close Preview3D and all import processes before cleanup.'
}

$nativeSource = @'
using System;
using System.Runtime.InteropServices;

public static class Preview3DProfileNative {
    [DllImport("userenv.dll", CharSet = CharSet.Unicode)]
    public static extern int DeriveAppContainerSidFromAppContainerName(string name, out IntPtr sid);

    [DllImport("userenv.dll", CharSet = CharSet.Unicode)]
    public static extern int DeleteAppContainerProfile(string name);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool ConvertSidToStringSid(IntPtr sid, out IntPtr textSid);

    [DllImport("kernel32.dll")]
    public static extern IntPtr LocalFree(IntPtr value);

    [DllImport("advapi32.dll")]
    public static extern IntPtr FreeSid(IntPtr sid);
}
'@

Add-Type -TypeDefinition $nativeSource

function Get-ProfileSid([string]$ProfileName) {
    $sid = [IntPtr]::Zero
    $hr = [Preview3DProfileNative]::DeriveAppContainerSidFromAppContainerName($ProfileName, [ref]$sid)
    if ($hr -lt 0 -or $sid -eq [IntPtr]::Zero) {
        throw ("Could not derive the $ProfileName AppContainer SID (HRESULT 0x{0:x8})." -f $hr)
    }
    $textSidPointer = [IntPtr]::Zero
    try {
        if (-not [Preview3DProfileNative]::ConvertSidToStringSid($sid, [ref]$textSidPointer)) {
            throw "Could not format the $ProfileName AppContainer SID."
        }
        return [System.Security.Principal.SecurityIdentifier]::new(
            [Runtime.InteropServices.Marshal]::PtrToStringUni($textSidPointer))
    } finally {
        if ($textSidPointer -ne [IntPtr]::Zero) { [void][Preview3DProfileNative]::LocalFree($textSidPointer) }
        [void][Preview3DProfileNative]::FreeSid($sid)
    }
}

function Remove-Profile([string]$ProfileName, $Identity) {
    foreach ($directory in @((Join-Path $PSScriptRoot 'worker'), (Join-Path $PSScriptRoot 'OpenUsdHost'), (Join-Path $PSScriptRoot 'StepHost'))) {
        if (Test-Path -LiteralPath $directory -PathType Container) {
            $acl = Get-Acl -LiteralPath $directory
            $acl.PurgeAccessRules($Identity)
            Set-Acl -LiteralPath $directory -AclObject $acl
        }
    }
    $deleteHr = [Preview3DProfileNative]::DeleteAppContainerProfile($ProfileName)
    # HRESULT_FROM_WIN32(ERROR_NOT_FOUND) means cleanup was already complete.
    if ($deleteHr -lt 0 -and $deleteHr -ne [int]0x80070490) {
        throw ("Could not delete the $ProfileName AppContainer profile (HRESULT 0x{0:x8})." -f $deleteHr)
    }
}

$workerSid = Get-ProfileSid 'Binbuf.Preview3D.ImportWorker'
$hostSid = Get-ProfileSid 'Binbuf.Preview3D.ImportHost'
$stepSid = Get-ProfileSid 'Binbuf.Preview3D.StepHost'
Remove-Profile 'Binbuf.Preview3D.ImportWorker' $workerSid
Remove-Profile 'Binbuf.Preview3D.ImportHost' $hostSid
Remove-Profile 'Binbuf.Preview3D.StepHost' $stepSid
Write-Host 'Preview3D worker, OpenUSD-host, and STEP-host AppContainer profile cleanup is complete.'

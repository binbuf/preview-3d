[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

if (Get-Process -Name Preview3D, Preview3DImportWorker -ErrorAction SilentlyContinue) {
    throw 'Close Preview3D and Preview3DImportWorker before cleanup.'
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
$profileName = 'Binbuf.Preview3D.ImportWorker'
$sid = [IntPtr]::Zero
$hr = [Preview3DProfileNative]::DeriveAppContainerSidFromAppContainerName($profileName, [ref]$sid)
if ($hr -lt 0 -or $sid -eq [IntPtr]::Zero) {
    throw ('Could not derive the Preview3D AppContainer SID (HRESULT 0x{0:x8}).' -f $hr)
}

try {
    $textSidPointer = [IntPtr]::Zero
    if (-not [Preview3DProfileNative]::ConvertSidToStringSid($sid, [ref]$textSidPointer)) {
        throw 'Could not format the Preview3D AppContainer SID.'
    }
    try {
        $textSid = [Runtime.InteropServices.Marshal]::PtrToStringUni($textSidPointer)
    } finally {
        [void][Preview3DProfileNative]::LocalFree($textSidPointer)
    }

    $workerDirectory = Join-Path $PSScriptRoot 'worker'
    if (Test-Path -LiteralPath $workerDirectory -PathType Container) {
        $acl = Get-Acl -LiteralPath $workerDirectory
        $identity = New-Object System.Security.Principal.SecurityIdentifier($textSid)
        $acl.PurgeAccessRules($identity)
        Set-Acl -LiteralPath $workerDirectory -AclObject $acl
    }

    $deleteHr = [Preview3DProfileNative]::DeleteAppContainerProfile($profileName)
    # HRESULT_FROM_WIN32(ERROR_NOT_FOUND) means cleanup was already complete.
    if ($deleteHr -lt 0 -and $deleteHr -ne [int]0x80070490) {
        throw ('Could not delete the Preview3D AppContainer profile (HRESULT 0x{0:x8}).' -f $deleteHr)
    }
    Write-Host 'Preview3D AppContainer profile cleanup is complete.'
} finally {
    [void][Preview3DProfileNative]::FreeSid($sid)
}

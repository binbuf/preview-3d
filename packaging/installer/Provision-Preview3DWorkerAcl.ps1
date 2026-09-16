[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$WorkerDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$worker = [System.IO.Path]::GetFullPath($WorkerDirectory).TrimEnd('\')
if (-not (Test-Path -LiteralPath $worker -PathType Container)) {
    throw "The Preview3D worker directory does not exist: $worker"
}

$nativeSource = @'
using System;
using System.Runtime.InteropServices;

public static class Preview3DProvisionNative {
    [DllImport("userenv.dll", CharSet = CharSet.Unicode)]
    public static extern int DeriveAppContainerSidFromAppContainerName(string name, out IntPtr sid);

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
$sidPointer = [IntPtr]::Zero
$hr = [Preview3DProvisionNative]::DeriveAppContainerSidFromAppContainerName($profileName, [ref]$sidPointer)
if ($hr -lt 0 -or $sidPointer -eq [IntPtr]::Zero) {
    throw ('Could not derive the Preview3D AppContainer SID (HRESULT 0x{0:x8}).' -f $hr)
}

try {
    $textSidPointer = [IntPtr]::Zero
    if (-not [Preview3DProvisionNative]::ConvertSidToStringSid($sidPointer, [ref]$textSidPointer)) {
        throw 'Could not format the Preview3D AppContainer SID.'
    }
    try {
        $textSid = [Runtime.InteropServices.Marshal]::PtrToStringUni($textSidPointer)
    } finally {
        [void][Preview3DProvisionNative]::LocalFree($textSidPointer)
    }

    $workerSid = New-Object System.Security.Principal.SecurityIdentifier($textSid)
    $allApplicationPackages = New-Object System.Security.Principal.SecurityIdentifier('S-1-15-2-1')
    $allRestrictedApplicationPackages = New-Object System.Security.Principal.SecurityIdentifier('S-1-15-2-2')
    $acl = Get-Acl -LiteralPath $worker

    # Program Files normally inherits broad application-package read access.
    # Preserve the ordinary system/admin/user entries, remove those two broad
    # AppContainer groups, and grant only Preview3D's deterministic worker SID.
    $acl.SetAccessRuleProtection($true, $true)
    $acl.PurgeAccessRules($allApplicationPackages)
    $acl.PurgeAccessRules($allRestrictedApplicationPackages)
    $acl.PurgeAccessRules($workerSid)

    $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
        $workerSid,
        [System.Security.AccessControl.FileSystemRights]::ReadAndExecute,
        [System.Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit',
        [System.Security.AccessControl.PropagationFlags]::None,
        [System.Security.AccessControl.AccessControlType]::Allow)
    [void]$acl.AddAccessRule($rule)
    Set-Acl -LiteralPath $worker -AclObject $acl

    $verified = Get-Acl -LiteralPath $worker
    $exactGrant = @($verified.Access | Where-Object {
        try {
            $_.IdentityReference.Translate([System.Security.Principal.SecurityIdentifier]).Value -eq $textSid -and
            $_.AccessControlType -eq [System.Security.AccessControl.AccessControlType]::Allow -and
            ($_.FileSystemRights -band [System.Security.AccessControl.FileSystemRights]::ReadAndExecute) -eq
                [System.Security.AccessControl.FileSystemRights]::ReadAndExecute -and
            ($_.InheritanceFlags -band [System.Security.AccessControl.InheritanceFlags]::ContainerInherit) -ne 0 -and
            ($_.InheritanceFlags -band [System.Security.AccessControl.InheritanceFlags]::ObjectInherit) -ne 0
        } catch { $false }
    })
    if ($exactGrant.Count -eq 0) {
        throw 'The Preview3D worker AppContainer ACL could not be verified after provisioning.'
    }

    Write-Host "Provisioned Preview3D worker ACL for $textSid on '$worker'."
} finally {
    [void][Preview3DProvisionNative]::FreeSid($sidPointer)
}

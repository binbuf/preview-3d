[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$WorkerDirectory,

    [Parameter(Mandatory = $true)]
    [string]$OpenUsdHostDirectory,

    [Parameter(Mandatory = $true)]
    [string]$StepHostDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$worker = [System.IO.Path]::GetFullPath($WorkerDirectory).TrimEnd('\')
$openUsdHost = [System.IO.Path]::GetFullPath($OpenUsdHostDirectory).TrimEnd('\')
$stepHost = [System.IO.Path]::GetFullPath($StepHostDirectory).TrimEnd('\')
if (-not (Test-Path -LiteralPath $worker -PathType Container)) {
    throw "The Preview3D worker directory does not exist: $worker"
}
if (-not (Test-Path -LiteralPath $openUsdHost -PathType Container)) {
    throw "The Preview3D OpenUSD host directory does not exist: $openUsdHost"
}
if (-not (Test-Path -LiteralPath $stepHost -PathType Container)) {
    throw "The Preview3D STEP host directory does not exist: $stepHost"
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

function Get-ProfileSid([string]$ProfileName) {
    $sidPointer = [IntPtr]::Zero
    $hr = [Preview3DProvisionNative]::DeriveAppContainerSidFromAppContainerName($ProfileName, [ref]$sidPointer)
    if ($hr -lt 0 -or $sidPointer -eq [IntPtr]::Zero) {
        throw ("Could not derive the $ProfileName AppContainer SID (HRESULT 0x{0:x8})." -f $hr)
    }
    $textSidPointer = [IntPtr]::Zero
    try {
        if (-not [Preview3DProvisionNative]::ConvertSidToStringSid($sidPointer, [ref]$textSidPointer)) {
            throw "Could not format the $ProfileName AppContainer SID."
        }
        $textSid = [Runtime.InteropServices.Marshal]::PtrToStringUni($textSidPointer)
        return [System.Security.Principal.SecurityIdentifier]::new($textSid)
    } finally {
        if ($textSidPointer -ne [IntPtr]::Zero) { [void][Preview3DProvisionNative]::LocalFree($textSidPointer) }
        [void][Preview3DProvisionNative]::FreeSid($sidPointer)
    }
}

function Set-PrivatePayloadAcl([string]$Directory, $AllowedSid, [object[]]$OtherSids) {
    $allApplicationPackages = New-Object System.Security.Principal.SecurityIdentifier('S-1-15-2-1')
    $allRestrictedApplicationPackages = New-Object System.Security.Principal.SecurityIdentifier('S-1-15-2-2')
    $acl = Get-Acl -LiteralPath $Directory

    # Preserve ordinary system/admin/user entries, but remove broad package
    # grants and every other product package SID before adding exactly one
    # identity, so no import sandbox can read a sibling payload.
    $acl.SetAccessRuleProtection($true, $true)
    $acl.PurgeAccessRules($allApplicationPackages)
    $acl.PurgeAccessRules($allRestrictedApplicationPackages)
    $acl.PurgeAccessRules($AllowedSid)
    foreach ($sid in $OtherSids) { $acl.PurgeAccessRules($sid) }

    $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
        $AllowedSid,
        [System.Security.AccessControl.FileSystemRights]::ReadAndExecute,
        [System.Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit',
        [System.Security.AccessControl.PropagationFlags]::None,
        [System.Security.AccessControl.AccessControlType]::Allow)
    [void]$acl.AddAccessRule($rule)
    Set-Acl -LiteralPath $Directory -AclObject $acl

    $verified = Get-Acl -LiteralPath $Directory
    $exactGrant = @($verified.Access | Where-Object {
        try {
            $_.IdentityReference.Translate([System.Security.Principal.SecurityIdentifier]).Value -eq $AllowedSid.Value -and
            $_.AccessControlType -eq [System.Security.AccessControl.AccessControlType]::Allow -and
            ($_.FileSystemRights -band [System.Security.AccessControl.FileSystemRights]::ReadAndExecute) -eq
                [System.Security.AccessControl.FileSystemRights]::ReadAndExecute -and
            ($_.InheritanceFlags -band [System.Security.AccessControl.InheritanceFlags]::ContainerInherit) -ne 0 -and
            ($_.InheritanceFlags -band [System.Security.AccessControl.InheritanceFlags]::ObjectInherit) -ne 0
        } catch { $false }
    })
    if ($exactGrant.Count -eq 0) {
        throw "The Preview3D private payload ACL could not be verified on '$Directory'."
    }
    foreach ($sid in $OtherSids) {
        $otherGrant = @($verified.Access | Where-Object {
            try { $_.IdentityReference.Translate([System.Security.Principal.SecurityIdentifier]).Value -eq $sid.Value }
            catch { $false }
        })
        if ($otherGrant.Count -ne 0) { throw "Another importer SID retained access to '$Directory'." }
    }
}

$workerSid = Get-ProfileSid 'Binbuf.Preview3D.ImportWorker'
$hostSid = Get-ProfileSid 'Binbuf.Preview3D.ImportHost'
$stepSid = Get-ProfileSid 'Binbuf.Preview3D.StepHost'
Set-PrivatePayloadAcl $worker $workerSid @($hostSid, $stepSid)
Set-PrivatePayloadAcl $openUsdHost $hostSid @($workerSid, $stepSid)
Set-PrivatePayloadAcl $stepHost $stepSid @($workerSid, $hostSid)
Write-Host "Provisioned isolated Preview3D payload ACLs for worker $($workerSid.Value), host $($hostSid.Value), and STEP host $($stepSid.Value)."

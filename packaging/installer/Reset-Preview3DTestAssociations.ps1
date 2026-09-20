<#
.SYNOPSIS
Removes stale per-user Preview3D file-association state before installer tests.

.DESCRIPTION
Developer builds or an earlier Open With selection can leave an HKCU
Applications\Preview3D.exe command that overrides the installer's HKLM command.
This script removes only Preview3D-owned per-user registration and Preview3D.exe
entries from the supported extensions' Open With history. It does not
remove other applications, source files, settings, the installed product, or a
Windows-protected UserChoice value.

Close Preview3D first. Uninstall the product separately when testing a clean
install lifecycle. Use -WhatIf to preview every mutation.
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$applicationExe = 'Preview3D.exe'
$applicationName = '3D Preview'
$extensions = @('.glb', '.gltf', '.stl', '.ply', '.obj', '.fbx', '.3mf', '.usd', '.usda', '.usdc', '.usdz', '.step', '.stp')
$progIds = @(
    'Binbuf.Preview3D.glTF.1',
    'Binbuf.Preview3D.STL.1',
    'Binbuf.Preview3D.PLY.1',
    'Binbuf.Preview3D.OBJ.1',
    'Binbuf.Preview3D.FBX.1',
    'Binbuf.Preview3D.ThreeMF.1',
    'Binbuf.Preview3D.USD.1',
    'Binbuf.Preview3D.STEP.1'
)

if (Get-Process -Name Preview3D, Preview3DImportWorker, Preview3DImportHost, Preview3DStepHost -ErrorAction SilentlyContinue) {
    throw 'Close Preview3D and all import processes before resetting association test state.'
}

function Remove-RegistryKey([string]$Path) {
    if ((Test-Path -LiteralPath $Path) -and $PSCmdlet.ShouldProcess($Path, 'Remove registry key')) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Remove-RegistryValue([string]$Path, [string]$Name) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $key = Get-Item -LiteralPath $Path
    try {
        if ($Name -notin @($key.GetValueNames())) { return }
    } finally {
        $key.Close()
    }
    if ($PSCmdlet.ShouldProcess("$Path [$Name]", 'Remove registry value')) {
        Remove-ItemProperty -LiteralPath $Path -Name $Name -Force
    }
}

function Remove-Preview3DOpenWithEntry([string]$Extension) {
    $path = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$Extension\OpenWithList"
    if (-not (Test-Path -LiteralPath $path)) { return }

    $key = Get-Item -LiteralPath $path
    try {
        $matchingNames = @($key.GetValueNames() | Where-Object {
            $_ -ne 'MRUList' -and
            [string]::Equals([string]$key.GetValue($_), $applicationExe,
                [System.StringComparison]::OrdinalIgnoreCase)
        })
        $mruList = [string]$key.GetValue('MRUList', '')
    } finally {
        $key.Close()
    }

    if ($matchingNames.Count -eq 0) { return }
    if (-not $PSCmdlet.ShouldProcess($path, "Remove $applicationExe Open With entries")) { return }

    foreach ($name in $matchingNames) {
        Remove-ItemProperty -LiteralPath $path -Name $name -Force
        $mruList = $mruList.Replace($name, '')
    }
    if ([string]::IsNullOrEmpty($mruList)) {
        Remove-ItemProperty -LiteralPath $path -Name 'MRUList' -Force -ErrorAction SilentlyContinue
    } else {
        Set-ItemProperty -LiteralPath $path -Name 'MRUList' -Value $mruList
    }
}

# HKCU takes precedence in HKCR. This is the stale entry that caused the
# installed machine command to be shadowed by a repository Debug executable.
Remove-RegistryKey 'Registry::HKEY_CURRENT_USER\Software\Classes\Applications\Preview3D.exe'

# Remove any older per-user variant of the product's current machine
# registration. Settings are file-backed under LocalAppData and are untouched.
foreach ($progId in $progIds) {
    Remove-RegistryKey "Registry::HKEY_CURRENT_USER\Software\Classes\$progId"
}
Remove-RegistryKey 'Registry::HKEY_CURRENT_USER\Software\Binbuf\Preview3D\Capabilities'
Remove-RegistryValue 'Registry::HKEY_CURRENT_USER\Software\RegisteredApplications' $applicationName
Remove-RegistryKey 'Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\App Paths\Preview3D.exe'

foreach ($extension in $extensions) {
    foreach ($progId in $progIds) {
        Remove-RegistryValue "Registry::HKEY_CURRENT_USER\Software\Classes\$extension\OpenWithProgids" $progId
        Remove-RegistryValue "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$extension\OpenWithProgids" $progId
    }
    Remove-Preview3DOpenWithEntry $extension
}

# These toasts are disposable Shell bookkeeping for this product only.
foreach ($extension in $extensions) {
    foreach ($progId in $progIds) {
        Remove-RegistryValue 'Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\ApplicationAssociationToasts' "${progId}_$extension"
    }
    Remove-RegistryValue 'Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\ApplicationAssociationToasts' "Applications\Preview3D.exe_$extension"
}

if (-not $WhatIfPreference) {
    if (-not ('Preview3DAssociationNative' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Preview3DAssociationNative {
    [DllImport("shell32.dll")]
    public static extern void SHChangeNotify(uint eventId, uint flags, IntPtr item1, IntPtr item2);
}
'@
    }
    [Preview3DAssociationNative]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
}

$effectiveCommandPath = 'Registry::HKEY_CLASSES_ROOT\Applications\Preview3D.exe\shell\open\command'
$effectiveCommand = if (Test-Path -LiteralPath $effectiveCommandPath) {
    [string](Get-Item -LiteralPath $effectiveCommandPath).GetValue('')
} else {
    '<not registered>'
}

if ($WhatIfPreference) {
    Write-Host 'Preview3D per-user association cleanup preview complete; no registry state was changed.'
} else {
    Write-Host 'Preview3D per-user association cleanup complete.'
}
Write-Host "Effective Applications command: $effectiveCommand"
Write-Host 'Windows UserChoice values were preserved. Select 3D Preview in Default Apps after installing.'

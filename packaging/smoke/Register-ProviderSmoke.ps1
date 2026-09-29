# T22/T23 developer/QA-local smoke registration (STL + PLY).
#
# Registers the product thumbnail CLSIDs and the T03-validated extension ShellEx
# mappings so the Shell routes .stl/.ply thumbnails to
# Preview3DThumbnailProvider.dll. This is a minimal local smoke, NOT the T41
# installer: it performs no conflict/repair/uninstall policy, and never sets
# DisableProcessIsolation.
#
# Every touched key is backed up first; Unregister-ProviderSmoke.ps1 restores the
# exact pre-registration state. Defaults to HKCU (per-user, no elevation); pass
# -Scope HKLM for the machine-level shape (needs an elevated shell).
#
# Usage:
#   pwsh -File packaging\smoke\Register-ProviderSmoke.ps1 -DllPath <staged dll>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DllPath,
    [ValidateSet('HKCU', 'HKLM')][string]$Scope = 'HKCU'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$thumbnailHandlerGuid = '{E357FCCD-A995-4576-B01F-234630154E96}'
$classesRoot = "${Scope}:\Software\Classes"

# Frozen product identities (thumbnail-provider/FamilyRouting.h); T23 adds PLY
# alongside the T22 STL entry.
$families = @(
    [ordered]@{
        Name      = 'STL'
        Clsid     = '{BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9}'
        AppId     = '{BFC86E1A-55C1-4C2D-AA36-3C25DECF3010}'
        Extension = '.stl'
        Display   = 'Preview 3D STL Thumbnail Provider'
    },
    [ordered]@{
        Name      = 'PLY'
        Clsid     = '{F4DC6119-E235-4BAC-8089-54EDD84F8492}'
        AppId     = '{F4DC6119-E235-4BAC-8089-54EDD84F8493}'
        Extension = '.ply'
        Display   = 'Preview 3D PLY Thumbnail Provider'
    }
)

$resolvedDll = if (Test-Path -LiteralPath $DllPath) {
    (Resolve-Path -LiteralPath $DllPath).Path
} else {
    throw "Provider DLL not found: $DllPath. Stage it first with Stage-ProviderSmoke.ps1."
}

$stateDir = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'artifacts\smoke'
$backupDir = Join-Path $stateDir 'backup'
New-Item -ItemType Directory -Force -Path $stateDir, $backupDir | Out-Null

# Keys this script owns. Extension/shellex keys are backed up so cleanup can
# remove them only when this smoke created them.
$keys = @()
foreach ($family in $families) {
    $keys += "$classesRoot\CLSID\$($family.Clsid)"
    $keys += "$classesRoot\AppID\$($family.AppId)"
    $keys += "$classesRoot\$($family.Extension)\shellex\$thumbnailHandlerGuid"
    $keys += "$classesRoot\$($family.Extension)\shellex"
    $keys += "$classesRoot\$($family.Extension)"
}

$regRoot = if ($Scope -eq 'HKCU') { 'HKCU' } else { 'HKLM' }
$manifest = @()
foreach ($key in $keys) {
    $safe = ($key -replace '[:\\{}]', '_')
    $file = Join-Path $backupDir "$safe.reg"
    if (Test-Path $key) {
        # reg.exe wants the hive without the PowerShell provider colon.
        $regKey = $key -replace "^$Scope`:", $regRoot
        & reg.exe export $regKey $file /y | Out-Null
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $file)) {
            throw "Failed to back up $key; refusing to register without a restorable backup."
        }
        $manifest += [ordered]@{ key = $key; existed = $true; backup = $file }
    } else {
        $manifest += [ordered]@{ key = $key; existed = $false; backup = $file }
    }
}
$manifest | ConvertTo-Json | Set-Content -Path (Join-Path $stateDir "backup-manifest-$Scope.json")

foreach ($family in $families) {
    $clsid = $family.Clsid
    $appId = $family.AppId

    # CLSID + InprocServer32 (ThreadingModel=Apartment), plus AppID/DllSurrogate so
    # an explicit CLSCTX_LOCAL_SERVER activation also routes into DllHost.exe (the
    # Shell path isolates by default regardless; ADR-0008).
    New-Item -Force -Path "$classesRoot\CLSID\$clsid\InprocServer32" | Out-Null
    Set-Item -Path "$classesRoot\CLSID\$clsid" -Value $family.Display
    Set-ItemProperty -Path "$classesRoot\CLSID\$clsid" -Name 'AppID' -Value $appId
    Set-Item -Path "$classesRoot\CLSID\$clsid\InprocServer32" -Value $resolvedDll
    Set-ItemProperty -Path "$classesRoot\CLSID\$clsid\InprocServer32" -Name 'ThreadingModel' -Value 'Apartment'

    New-Item -Force -Path "$classesRoot\AppID\$appId" | Out-Null
    Set-Item -Path "$classesRoot\AppID\$appId" -Value $family.Display
    Set-ItemProperty -Path "$classesRoot\AppID\$appId" -Name 'DllSurrogate' -Value '' -Type String
    # Belt and braces: ensure no isolation opt-out can be inherited from a previous run.
    Remove-ItemProperty -Path "$classesRoot\CLSID\$clsid" -Name 'DisableProcessIsolation' -ErrorAction SilentlyContinue
    Remove-ItemProperty -Path "$classesRoot\AppID\$appId" -Name 'DisableProcessIsolation' -ErrorAction SilentlyContinue

    # T03-validated extension-level ShellEx thumbnail handler mapping.
    New-Item -Force -Path "$classesRoot\$($family.Extension)\shellex\$thumbnailHandlerGuid" | Out-Null
    Set-Item -Path "$classesRoot\$($family.Extension)\shellex\$thumbnailHandlerGuid" -Value $clsid
}

# Record the exact keys and values written.
$record = Join-Path $stateDir "registered-keys-$Scope.txt"
$dump = @("scope=$Scope dll=$resolvedDll")
foreach ($family in $families) {
    $dump += "= $regRoot\Software\Classes\CLSID\$($family.Clsid)"
    $dump += (& reg.exe query "$regRoot\Software\Classes\CLSID\$($family.Clsid)" /s | Out-String)
    $dump += "= $regRoot\Software\Classes\AppID\$($family.AppId)"
    $dump += (& reg.exe query "$regRoot\Software\Classes\AppID\$($family.AppId)" /s | Out-String)
    $dump += "= $regRoot\Software\Classes\$($family.Extension)\shellex\$thumbnailHandlerGuid"
    $dump += (& reg.exe query "$regRoot\Software\Classes\$($family.Extension)\shellex\$thumbnailHandlerGuid" /s | Out-String)
}
$dump | Set-Content -Path $record -Encoding UTF8

Write-Output "registered thumbnail CLSIDs under $Scope"
Write-Output "  DLL      : $resolvedDll"
foreach ($family in $families) {
    Write-Output "  $($family.Name): CLSID=$($family.Clsid) AppID=$($family.AppId) ShellEx=$($family.Extension)\shellex\$thumbnailHandlerGuid"
}
Write-Output "  key record: $record"
Write-Output "  backups   : $backupDir"
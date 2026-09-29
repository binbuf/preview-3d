# T22 developer/QA-local smoke registration (STL only).
#
# Registers the product STL thumbnail CLSID and the T03-validated extension
# ShellEx mapping so the Shell routes .stl thumbnails to Preview3DThumbnailProvider.dll.
# This is a minimal local smoke, NOT the T41 installer: it writes one family only,
# performs no conflict/repair/uninstall policy, and never sets DisableProcessIsolation.
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

$providerClsid = '{BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9}'
$providerAppId = '{BFC86E1A-55C1-4C2D-AA36-3C25DECF3010}'
$thumbnailHandlerGuid = '{E357FCCD-A995-4576-B01F-234630154E96}'
$classesRoot = "${Scope}:\Software\Classes"

$resolvedDll = if (Test-Path -LiteralPath $DllPath) {
    (Resolve-Path -LiteralPath $DllPath).Path
} else {
    throw "Provider DLL not found: $DllPath. Stage it first with Stage-ProviderSmoke.ps1."
}

$stateDir = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'artifacts\smoke'
$backupDir = Join-Path $stateDir 'backup'
New-Item -ItemType Directory -Force -Path $stateDir, $backupDir | Out-Null

# Keys this script owns. `.stl` and `.stl\shellex` are backed up so cleanup can
# remove them only when this smoke created them.
$keys = @(
    "$classesRoot\CLSID\$providerClsid",
    "$classesRoot\AppID\$providerAppId",
    "$classesRoot\.stl\shellex\$thumbnailHandlerGuid",
    "$classesRoot\.stl\shellex",
    "$classesRoot\.stl"
)

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

# CLSID + InprocServer32 (ThreadingModel=Apartment), plus AppID/DllSurrogate so an
# explicit CLSCTX_LOCAL_SERVER activation also routes into DllHost.exe (the Shell
# path isolates by default regardless; ADR-0008).
New-Item -Force -Path "$classesRoot\CLSID\$providerClsid\InprocServer32" | Out-Null
Set-Item -Path "$classesRoot\CLSID\$providerClsid" -Value 'Preview 3D STL Thumbnail Provider'
Set-ItemProperty -Path "$classesRoot\CLSID\$providerClsid" -Name 'AppID' -Value $providerAppId
Set-Item -Path "$classesRoot\CLSID\$providerClsid\InprocServer32" -Value $resolvedDll
Set-ItemProperty -Path "$classesRoot\CLSID\$providerClsid\InprocServer32" -Name 'ThreadingModel' -Value 'Apartment'

New-Item -Force -Path "$classesRoot\AppID\$providerAppId" | Out-Null
Set-Item -Path "$classesRoot\AppID\$providerAppId" -Value 'Preview 3D STL Thumbnail Provider'
Set-ItemProperty -Path "$classesRoot\AppID\$providerAppId" -Name 'DllSurrogate' -Value '' -Type String
# Belt and braces: ensure no isolation opt-out can be inherited from a previous run.
Remove-ItemProperty -Path "$classesRoot\CLSID\$providerClsid" -Name 'DisableProcessIsolation' -ErrorAction SilentlyContinue
Remove-ItemProperty -Path "$classesRoot\AppID\$providerAppId" -Name 'DisableProcessIsolation' -ErrorAction SilentlyContinue

# T03-validated extension-level ShellEx thumbnail handler mapping.
New-Item -Force -Path "$classesRoot\.stl\shellex\$thumbnailHandlerGuid" | Out-Null
Set-Item -Path "$classesRoot\.stl\shellex\$thumbnailHandlerGuid" -Value $providerClsid

# Record the exact keys and values written.
$record = Join-Path $stateDir "registered-keys-$Scope.txt"
$dump = @(
    "scope=$Scope dll=$resolvedDll",
    "= $regRoot\Software\Classes\CLSID\$providerClsid",
    (& reg.exe query "$regRoot\Software\Classes\CLSID\$providerClsid" /s | Out-String),
    "= $regRoot\Software\Classes\AppID\$providerAppId",
    (& reg.exe query "$regRoot\Software\Classes\AppID\$providerAppId" /s | Out-String),
    "= $regRoot\Software\Classes\.stl\shellex\$thumbnailHandlerGuid",
    (& reg.exe query "$regRoot\Software\Classes\.stl\shellex\$thumbnailHandlerGuid" /s | Out-String)
)
$dump | Set-Content -Path $record -Encoding UTF8

Write-Output "registered STL thumbnail CLSID $providerClsid under $Scope"
Write-Output "  DLL            : $resolvedDll"
Write-Output "  AppID          : $providerAppId (DllSurrogate=empty REG_SZ)"
Write-Output "  ShellEx        : $regRoot\Software\Classes\.stl\shellex\$thumbnailHandlerGuid = $providerClsid"
Write-Output "  key record     : $record"
Write-Output "  backups        : $backupDir"
# SPIKE-8b (T03) probe registration.
#
# Registers the throwaway SurrogateProbe.dll under a dedicated scratch CLSID and a
# dedicated scratch extension. It NEVER writes the protected UserChoice value and
# never sets DisableProcessIsolation. Every touched key is backed up first so
# unregister-probe.ps1 restores the machine exactly.
#
# Defaults to HKCU (per-user, no elevation). Pass -Scope HKLM to attempt the
# machine-level shape the production installer will use; that requires an
# elevated (administrator) shell and is reported, not silently skipped.
#
# Usage:
#   pwsh -File register-probe.ps1 -DllPath x64\Release\SurrogateProbe.dll
#   pwsh -File register-probe.ps1 -DefaultProgId txtfile          # third-party default

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DllPath,
    [ValidateSet('HKCU', 'HKLM')][string]$Scope = 'HKCU',
    [string]$Extension = '.z3dprobe',
    [string]$DefaultProgId = '',
    [string]$ProbeDllOverride = ''
)

$ErrorActionPreference = 'Stop'

$probeClsid = '{C7A5B3E1-9D24-4F88-A1B6-2E5C7D9F0A31}'
$probeAppId = '{C7A5B3E1-9D24-4F88-A1B6-2E5C7D9F0A32}'
$thumbnailHandlerClsid = '{E357FCCD-A995-4576-B01F-234630154E96}'
$classesRoot = "${Scope}:\Software\Classes"
$configRoot = 'HKCU:\Software\Preview3DThumbnailSpike'

$resolvedDll = if ($ProbeDllOverride) { $ProbeDllOverride } elseif (Test-Path -LiteralPath $DllPath) {
    (Resolve-Path -LiteralPath $DllPath).Path
} else { $DllPath }

$scratchDir = Join-Path $PSScriptRoot '.probe-state'
$backupDir = Join-Path $scratchDir 'backup'
New-Item -ItemType Directory -Force -Path $scratchDir, $backupDir | Out-Null

$keys = @(
    "HKCU:\Software\Classes\CLSID\$probeClsid",
    "HKCU:\Software\Classes\AppID\$probeAppId",
    "HKCU:\Software\Classes\$Extension",
    $configRoot
)
$backupManifest = @()
foreach ($key in $keys) {
    $safe = ($key -replace '[:\\{}]', '_')
    $file = Join-Path $backupDir "$safe.reg"
    if (Test-Path $key) {
        & reg.exe export $key $file /y | Out-Null
        $backupManifest += [ordered]@{ key = $key; existed = $true; backup = $file }
    } else {
        $backupManifest += [ordered]@{ key = $key; existed = $false; backup = $file }
    }
}
$backupManifest | ConvertTo-Json | Set-Content -Path (Join-Path $scratchDir 'backup-manifest.json')

# InprocServer32 + AppID/DllSurrogate. The AppID is what routes a
# CLSCTX_LOCAL_SERVER activation into DllHost.exe; no DisableProcessIsolation.
New-Item -Force -Path "$classesRoot\CLSID\$probeClsid\InprocServer32" | Out-Null
Set-Item -Path "$classesRoot\CLSID\$probeClsid" -Value 'Preview 3D Surrogate Probe (T03 spike)'
Set-ItemProperty -Path "$classesRoot\CLSID\$probeClsid" -Name 'AppID' -Value $probeAppId
Set-Item -Path "$classesRoot\CLSID\$probeClsid\InprocServer32" -Value $resolvedDll
Set-ItemProperty -Path "$classesRoot\CLSID\$probeClsid\InprocServer32" -Name 'ThreadingModel' -Value 'Apartment'

New-Item -Force -Path "$classesRoot\AppID\$probeAppId" | Out-Null
Set-Item -Path "$classesRoot\AppID\$probeAppId" -Value 'Preview 3D Surrogate Probe (T03 spike)'
Set-ItemProperty -Path "$classesRoot\AppID\$probeAppId" -Name 'DllSurrogate' -Value '' -Type String

# Extension ShellEx thumbnail mapping (the proposed production shape).
New-Item -Force -Path "$classesRoot\$Extension\shellex\$thumbnailHandlerClsid" | Out-Null
Set-Item -Path "$classesRoot\$Extension\shellex\$thumbnailHandlerClsid" -Value $probeClsid
if ($DefaultProgId) {
    Set-Item -Path "$classesRoot\$Extension" -Value $DefaultProgId
} elseif (-not (Get-ItemProperty -Path "$classesRoot\$Extension" -ErrorAction SilentlyContinue)) {
    Set-Item -Path "$classesRoot\$Extension" -Value 'Preview3D.T03Probe'
}

# Probe run configuration lives per-user regardless of the registration scope.
New-Item -Force -Path $configRoot | Out-Null

Write-Output "registered probe CLSID $probeClsid under $Scope"
Write-Output "  DllPath        : $resolvedDll"
Write-Output "  AppID          : $probeAppId (DllSurrogate=empty)"
if ($DefaultProgId) { Write-Output "  default ProgID : $DefaultProgId (per-user classes default; NOT UserChoice)" }
Write-Output "  extension      : $Extension"
Write-Output "  backups        : $backupDir"
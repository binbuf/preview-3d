# SPIKE-8b (T03) probe registration cleanup.
#
# Removes every key register-probe.ps1 created and, where a key pre-existed,
# re-imports the exact backup captured before registration. Also removes the
# probe's per-user run configuration, scratch extension file and probe log.
#
# Usage:
#   pwsh -File unregister-probe.ps1

[CmdletBinding()]
param(
    [string]$Extension = '.z3dprobe',
    [string]$LogPath = (Join-Path $env:TEMP 'preview3d-surrogate-probe.log')
)

$ErrorActionPreference = 'Stop'

$probeClsid = '{C7A5B3E1-9D24-4F88-A1B6-2E5C7D9F0A31}'
$probeAppId = '{C7A5B3E1-9D24-4F88-A1B6-2E5C7D9F0A32}'

$stateDir = Join-Path $PSScriptRoot '.probe-state'
$manifestPath = Join-Path $stateDir 'backup-manifest.json'

# Always remove the keys we own, machine or user scope.
$ownedKeys = @(
    "HKCU:\Software\Classes\CLSID\$probeClsid",
    "HKCU:\Software\Classes\AppID\$probeAppId",
    "HKLM:\Software\Classes\CLSID\$probeClsid",
    "HKLM:\Software\Classes\AppID\$probeAppId",
    "HKCU:\Software\Classes\$Extension\shellex\{E357FCCD-A995-4576-B01F-234630154E96}",
    "HKLM:\Software\Classes\$Extension\shellex\{E357FCCD-A995-4576-B01F-234630154E96}",
    'HKCU:\Software\Preview3DThumbnailSpike'
)
foreach ($key in $ownedKeys) {
    if (Test-Path $key) {
        Remove-Item -Path $key -Recurse -Force
        Write-Output "removed $key"
    }
}

# Remove the extension key only if we created it (no backup existed).
if (Test-Path $manifestPath) {
    $manifest = Get-Content -Raw -Path $manifestPath | ConvertFrom-Json
    foreach ($entry in $manifest) {
        if ($entry.existed -and (Test-Path -LiteralPath $entry.backup)) {
            & reg.exe import $entry.backup | Out-Null
            Write-Output "restored $($entry.key) from backup"
        } elseif (-not $entry.existed -and (Test-Path $entry.key)) {
            Remove-Item -Path $entry.key -Recurse -Force
            Write-Output "removed $($entry.key)"
        }
    }
} elseif (Test-Path "HKCU:\Software\Classes\$Extension") {
    Remove-Item -Path "HKCU:\Software\Classes\$Extension" -Recurse -Force
    Write-Output "removed HKCU:\Software\Classes\$Extension (no manifest)"
}

if (Test-Path -LiteralPath $LogPath) {
    Remove-Item -LiteralPath $LogPath -Force
    Write-Output "removed probe log $LogPath"
}

if (Test-Path $stateDir) {
    Remove-Item -Path $stateDir -Recurse -Force
    Write-Output "removed $stateDir"
}

Write-Output 'probe registration cleanup complete'
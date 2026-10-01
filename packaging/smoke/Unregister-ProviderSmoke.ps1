# T22/T23 developer/QA-local smoke cleanup (STL + PLY).
#
# Removes every key Register-ProviderSmoke.ps1 created and restores the exact
# pre-registration state from the captured backups. Also stops any DllHost
# surrogate still holding the staged provider so the stage can be deleted.
#
# Usage:
#   pwsh -File packaging\smoke\Unregister-ProviderSmoke.ps1 [-Scope HKCU|HKLM]

[CmdletBinding()]
param(
    [ValidateSet('HKCU', 'HKLM')][string]$Scope = 'HKCU'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$thumbnailHandlerGuid = '{E357FCCD-A995-4576-B01F-234630154E96}'
$classesRoot = "${Scope}:\Software\Classes"

$families = @(
    [ordered]@{
        Name      = 'STL'
        Clsid     = '{BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9}'
        AppId     = '{BFC86E1A-55C1-4C2D-AA36-3C25DECF3010}'
        Extension = '.stl'
    },
    [ordered]@{
        Name      = 'PLY'
        Clsid     = '{F4DC6119-E235-4BAC-8089-54EDD84F8492}'
        AppId     = '{F4DC6119-E235-4BAC-8089-54EDD84F8493}'
        Extension = '.ply'
    },
    [ordered]@{
        Name      = 'glTF'
        Clsid     = '{A592F425-EA68-4C88-BB96-020805D4BE56}'
        AppId     = '{A592F425-EA68-4C88-BB96-020805D4BE57}'
        Extension = '.glb'
    },
    [ordered]@{
        Name      = 'FBX'
        Clsid     = '{FBC218D4-FD2C-41DF-B168-7F3B9E53C84E}'
        AppId     = '{FBC218D4-FD2C-41DF-B168-7F3B9E53C84F}'
        Extension = '.fbx'
    },
    [ordered]@{
        Name      = '3MF'
        Clsid     = '{D8389A63-8526-454A-9892-72F3149484B9}'
        AppId     = '{D8389A63-8526-454A-9892-72F3149484BA}'
        Extension = '.3mf'
    },
    [ordered]@{
        Name      = 'USD'
        Clsid     = '{E938BC70-4C08-4446-A15D-EE31576BFB48}'
        AppId     = '{E938BC70-4C08-4446-A15D-EE31576BFB49}'
        Extension = '.usd'
    },
    [ordered]@{
        Name      = 'USD'
        Clsid     = '{E938BC70-4C08-4446-A15D-EE31576BFB48}'
        AppId     = '{E938BC70-4C08-4446-A15D-EE31576BFB49}'
        Extension = '.usda'
    },
    [ordered]@{
        Name      = 'USD'
        Clsid     = '{E938BC70-4C08-4446-A15D-EE31576BFB48}'
        AppId     = '{E938BC70-4C08-4446-A15D-EE31576BFB49}'
        Extension = '.usdc'
    },
    [ordered]@{
        Name      = 'USD'
        Clsid     = '{E938BC70-4C08-4446-A15D-EE31576BFB48}'
        AppId     = '{E938BC70-4C08-4446-A15D-EE31576BFB49}'
        Extension = '.usdz'
    },
    [ordered]@{
        Name      = 'STEP'
        Clsid     = '{6EE961AC-AC3B-4958-A898-E30523FEE79D}'
        AppId     = '{6EE961AC-AC3B-4958-A898-E30523FEE79E}'
        Extension = '.step'
    },
    [ordered]@{
        Name      = 'STEP'
        Clsid     = '{6EE961AC-AC3B-4958-A898-E30523FEE79D}'
        AppId     = '{6EE961AC-AC3B-4958-A898-E30523FEE79E}'
        Extension = '.stp'
    }
)

$stateDir = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'artifacts\smoke'
$manifestPath = Join-Path $stateDir "backup-manifest-$Scope.json"
$manifest = @{}
if (Test-Path -LiteralPath $manifestPath) {
    $entries = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    foreach ($entry in $entries) { $manifest[$entry.key] = $entry }
}

function Test-ManifestExisted([string]$Key) {
    return $manifest.ContainsKey($Key) -and $manifest[$Key].existed
}

function Restore-Or-Remove([string]$Key) {
    if (Test-ManifestExisted $Key) {
        $backup = $manifest[$Key].backup
        if (Test-Path -LiteralPath $backup) {
            & reg.exe import $backup | Out-Null
            Write-Output "restored $Key from backup"
            return
        }
    }
    if (Test-Path $Key) {
        Remove-Item -Path $Key -Recurse -Force
        Write-Output "removed $Key"
    }
}

# Stop only DllHost instances that loaded this provider, so a stale surrogate
# cannot keep the staged DLL locked or serve a removed registration.
foreach ($process in @(Get-Process -Name dllhost -ErrorAction SilentlyContinue)) {
    try {
        $loaded = @($process.Modules | Where-Object { $_.ModuleName -eq 'Preview3DThumbnailProvider.dll' })
        if ($loaded.Count -gt 0) {
            Stop-Process -Id $process.Id -Force -ErrorAction Stop
            Write-Output "stopped DllHost pid=$($process.Id) holding the provider"
        }
    } catch {
        # Access to another user's DllHost modules is denied; ignore it.
    }
}

foreach ($family in $families) {
    Restore-Or-Remove "$classesRoot\CLSID\$($family.Clsid)"
    Restore-Or-Remove "$classesRoot\AppID\$($family.AppId)"

    # The handler key and its parents: restore a pre-existing handler value, then
    # prune the shellex/extension keys only when this smoke created them.
    $handlerKey = "$classesRoot\$($family.Extension)\shellex\$thumbnailHandlerGuid"
    if (Test-ManifestExisted $handlerKey) {
        $backup = $manifest[$handlerKey].backup
        if (Test-Path -LiteralPath $backup) {
            & reg.exe import $backup | Out-Null
            Write-Output "restored $handlerKey from backup"
        }
    } elseif (Test-Path $handlerKey) {
        Remove-Item -Path $handlerKey -Recurse -Force
        Write-Output "removed $handlerKey"
    }

    $shellexKey = "$classesRoot\$($family.Extension)\shellex"
    if (-not (Test-ManifestExisted $shellexKey) -and (Test-Path $shellexKey)) {
        $children = @(Get-ChildItem -Path $shellexKey -ErrorAction SilentlyContinue)
        $values = @()
        $properties = @(Get-ItemProperty -Path $shellexKey -ErrorAction SilentlyContinue)
        if ($properties.Count -gt 0) {
            $values = @($properties |
                Get-Member -MemberType NoteProperty |
                Where-Object { $_.Name -notmatch '^PS' })
        }
        if ($children.Count -eq 0 -and $values.Count -eq 0) {
            Remove-Item -Path $shellexKey -Recurse -Force
            Write-Output "removed empty $shellexKey"
        }
    }

    $extensionKey = "$classesRoot\$($family.Extension)"
    if (-not (Test-ManifestExisted $extensionKey) -and (Test-Path $extensionKey)) {
        $children = @(Get-ChildItem -Path $extensionKey -ErrorAction SilentlyContinue)
        if ($children.Count -eq 0) {
            Remove-Item -Path $extensionKey -Recurse -Force
            Write-Output "removed empty $extensionKey"
        }
    }
}

$backupDir = Join-Path $stateDir 'backup'
if (Test-Path -LiteralPath $backupDir) {
    Remove-Item -LiteralPath $backupDir -Recurse -Force
    Write-Output "removed $backupDir"
}
foreach ($scratch in @($manifestPath, (Join-Path $stateDir "registered-keys-$Scope.txt"))) {
    if (Test-Path -LiteralPath $scratch) { Remove-Item -LiteralPath $scratch -Force }
}

Write-Output "STL/PLY/glTF/FBX/3MF/USD/STEP smoke registration cleanup complete"

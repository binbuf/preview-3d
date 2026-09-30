# T41 machine-level Explorer thumbnail-provider registration.
#
# This is the product-owned registration step the NSIS installer runs (ADR-0006,
# ADR-0007, ADR-0028). It writes, for the eight frozen family CLSIDs:
#
#   HKLM\Software\Classes\CLSID\{family-clsid}
#     (Default)            = "Preview 3D <family> Thumbnail Provider"
#     AppID                = {family-appid}
#     InprocServer32\
#       (Default)          = "<installed>\Preview3DThumbnailProvider.dll"
#       ThreadingModel     = "Apartment"
#   HKLM\Software\Classes\AppID\{family-appid}
#     (Default)            = "Preview 3D <family> Thumbnail Provider"
#     DllSurrogate         = ""
#
# and, for every direct extension, the T03/ADR-0008 validated mapping:
#
#   HKLM\Software\Classes\<extension>\shellex\{E357FCCD-...} = "{family-clsid}"
#
# The identities are frozen (design/05-thumbnail-provider.md) and must match
# thumbnail-provider/FamilyRouting.h; tests/app-smoke/thumbnail_registration.py
# fails if this table drifts from that header.
#
# Policy (design/08-installation-and-registration.md, ADR-0006/ADR-0028):
#   * non-clobber: a pre-existing non-product handler in an extension ShellEx
#     value is preserved and recorded as a conflict;
#   * repair is an idempotent re-run of this same action: missing product keys
#     are restored, foreign handlers are never overwritten;
#   * uninstall removes only product-owned keys/values;
#   * DisableProcessIsolation is never written and is removed if present;
#   * user defaults (UserChoice) are never read or written.
#
# The script is not a COM self-registration path: the provider DLL exports no
# DllRegisterServer/DllUnregisterServer.
#
# Usage (installer):
#   powershell -File Preview3DThumbnailRegistration.ps1 -Action Install `
#       -Scope HKLM -DllPath "$INSTDIR\Preview3DThumbnailProvider.dll"
#   powershell -File Preview3DThumbnailRegistration.ps1 -Action Uninstall -Scope HKLM `
#       -DllPath "$INSTDIR\Preview3DThumbnailProvider.dll"
#
# -ClassesRoot / -StateRoot exist so tests can run the exact same logic against a
# sandboxed per-user key without touching the real association store.

[CmdletBinding()]
param(
    [ValidateSet('Install', 'Uninstall')][string]$Action = 'Install',
    [ValidateSet('HKLM', 'HKCU')][string]$Scope = 'HKLM',
    [string]$DllPath = '',
    [string]$ClassesRoot = '',
    [string]$StateRoot = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

if ([string]::IsNullOrWhiteSpace($ClassesRoot)) {
    $ClassesRoot = "${Scope}:\Software\Classes"
}
if ([string]::IsNullOrWhiteSpace($StateRoot)) {
    $StateRoot = "${Scope}:\Software\Binbuf\Preview3D\Thumbnails"
}

# Frozen product identities. The CLSIDs and the handler GUID are never
# regenerated; the AppIDs are the ones the T03/T22-validated shape used for
# explicit CLSCTX_LOCAL_SERVER activation and are frozen here for the installer.
$thumbnailHandlerGuid = '{E357FCCD-A995-4576-B01F-234630154E96}'

$families = @(
    [ordered]@{
        Family       = 'glTF'
        Clsid        = '{A592F425-EA68-4C88-BB96-020805D4BE56}'
        AppId        = '{A592F425-EA68-4C88-BB96-020805D4BE57}'
        Display      = 'Preview 3D glTF Thumbnail Provider'
        Extensions   = @('.glb', '.gltf')
    },
    [ordered]@{
        Family       = 'STL'
        Clsid        = '{BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9}'
        AppId        = '{BFC86E1A-55C1-4C2D-AA36-3C25DECF3010}'
        Display      = 'Preview 3D STL Thumbnail Provider'
        Extensions   = @('.stl')
    },
    [ordered]@{
        Family       = 'PLY'
        Clsid        = '{F4DC6119-E235-4BAC-8089-54EDD84F8492}'
        AppId        = '{F4DC6119-E235-4BAC-8089-54EDD84F8493}'
        Display      = 'Preview 3D PLY Thumbnail Provider'
        Extensions   = @('.ply')
    },
    [ordered]@{
        Family       = 'OBJ'
        Clsid        = '{D4722752-C480-4D9C-BEBE-1A9B514A8846}'
        AppId        = '{D4722752-C480-4D9C-BEBE-1A9B514A8847}'
        Display      = 'Preview 3D OBJ Thumbnail Provider'
        Extensions   = @('.obj')
    },
    [ordered]@{
        Family       = 'FBX'
        Clsid        = '{FBC218D4-FD2C-41DF-B168-7F3B9E53C84E}'
        AppId        = '{FBC218D4-FD2C-41DF-B168-7F3B9E53C84F}'
        Display      = 'Preview 3D FBX Thumbnail Provider'
        Extensions   = @('.fbx')
    },
    [ordered]@{
        Family       = '3MF'
        Clsid        = '{D8389A63-8526-454A-9892-72F3149484B9}'
        AppId        = '{D8389A63-8526-454A-9892-72F3149484BA}'
        Display      = 'Preview 3D 3MF Thumbnail Provider'
        Extensions   = @('.3mf')
    },
    [ordered]@{
        Family       = 'USD'
        Clsid        = '{E938BC70-4C08-4446-A15D-EE31576BFB48}'
        AppId        = '{E938BC70-4C08-4446-A15D-EE31576BFB49}'
        Display      = 'Preview 3D USD Thumbnail Provider'
        Extensions   = @('.usd', '.usda', '.usdc', '.usdz')
    },
    [ordered]@{
        Family       = 'STEP'
        Clsid        = '{6EE961AC-AC3B-4958-A898-E30523FEE79D}'
        AppId        = '{6EE961AC-AC3B-4958-A898-E30523FEE79E}'
        Display      = 'Preview 3D STEP Thumbnail Provider'
        Extensions   = @('.step', '.stp')
    }
)

$productClsids = @($families | ForEach-Object { $_.Clsid })

function Get-RegistryDefaultValue([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $key = Get-Item -LiteralPath $Path
    try {
        $value = $key.GetValue('')
        if ($null -eq $value) { return '' }
        return [string]$value
    } finally {
        $key.Close()
    }
}

function Set-RegistryDefaultValue([string]$Path, [string]$Value) {
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -Force -Path $Path | Out-Null
    }
    Set-Item -LiteralPath $Path -Value $Value
}

function Remove-RegistryDefaultValue([string]$Path) {
    # The PowerShell provider's Remove-ItemProperty does not delete the unnamed
    # (default) value reliably, so open the key writable and delete it directly.
    if ($Path -notmatch '^(HKLM|HKCU):\\(.*)$') {
        throw "Unsupported registry path: $Path"
    }
    $hive = if ($matches[1] -eq 'HKLM') { [Microsoft.Win32.Registry]::LocalMachine } else { [Microsoft.Win32.Registry]::CurrentUser }
    $subKey = $matches[2]
    $key = $hive.OpenSubKey($subKey, $true)
    if ($null -eq $key) { return }
    try {
        if ($key.GetValueNames() -contains '') {
            $key.DeleteValue('', $false)
        }
    } finally {
        $key.Close()
    }
}

function Test-RegistryKeyEmpty([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $true }
    $key = Get-Item -LiteralPath $Path
    try {
        return (($key.GetValueNames().Count + $key.GetSubKeyNames().Count) -eq 0)
    } finally {
        $key.Close()
    }
}

function Remove-RegistryKeyIfEmpty([string]$Path) {
    if ((Test-Path -LiteralPath $Path) -and (Test-RegistryKeyEmpty $Path)) {
        Remove-Item -LiteralPath $Path -Force
    }
}

function Get-StateValue([string]$SubKey, [string]$Name) {
    $path = Join-Path $StateRoot $SubKey
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $key = Get-Item -LiteralPath $path
    try {
        if ($Name -notin @($key.GetValueNames())) { return $null }
        return [string]$key.GetValue($Name)
    } finally {
        $key.Close()
    }
}

function Set-StateValue([string]$SubKey, [string]$Name, [string]$Value) {
    $path = Join-Path $StateRoot $SubKey
    if (-not (Test-Path -LiteralPath $path)) {
        New-Item -Force -Path $path | Out-Null
    }
    New-ItemProperty -LiteralPath $path -Name $Name -Value $Value -Type String -Force | Out-Null
}

function Remove-StateValue([string]$SubKey, [string]$Name) {
    $path = Join-Path $StateRoot $SubKey
    if (-not (Test-Path -LiteralPath $path)) { return }
    Remove-ItemProperty -LiteralPath $path -Name $Name -Force -ErrorAction SilentlyContinue
}

function New-ProductState() {
    New-Item -Force -Path "$StateRoot\OwnedClsid" | Out-Null
    New-Item -Force -Path "$StateRoot\OwnedAppId" | Out-Null
    New-Item -Force -Path "$StateRoot\OwnedShellEx" | Out-Null
    # Conflicts are re-detected from live registry state on every run.
    if (Test-Path -LiteralPath "$StateRoot\Conflicts") {
        Remove-Item -LiteralPath "$StateRoot\Conflicts" -Recurse -Force
    }
    New-Item -Force -Path "$StateRoot\Conflicts" | Out-Null
    New-ItemProperty -LiteralPath $StateRoot -Name 'Schema' -Value 1 -PropertyType DWord -Force | Out-Null
}

function Install-ThumbnailRegistration() {
    if ([string]::IsNullOrWhiteSpace($DllPath)) {
        throw '-DllPath is required for -Action Install.'
    }

    New-ProductState
    $conflicts = @()
    $registered = 0

    foreach ($family in $families) {
        $clsidKey = "$ClassesRoot\CLSID\$($family.Clsid)"
        $inprocKey = "$clsidKey\InprocServer32"
        $appIdKey = "$ClassesRoot\AppID\$($family.AppId)"

        New-Item -Force -Path $inprocKey | Out-Null
        Set-RegistryDefaultValue $clsidKey $family.Display
        Set-ItemProperty -LiteralPath $clsidKey -Name 'AppID' -Value $family.AppId -Type String -Force
        Set-RegistryDefaultValue $inprocKey $DllPath
        Set-ItemProperty -LiteralPath $inprocKey -Name 'ThreadingModel' -Value 'Apartment' -Type String -Force

        New-Item -Force -Path $appIdKey | Out-Null
        Set-RegistryDefaultValue $appIdKey $family.Display
        Set-ItemProperty -LiteralPath $appIdKey -Name 'DllSurrogate' -Value '' -Type String -Force

        # Never allow an isolation opt-out to survive an install or repair.
        Remove-ItemProperty -LiteralPath $clsidKey -Name 'DisableProcessIsolation' -Force -ErrorAction SilentlyContinue
        Remove-ItemProperty -LiteralPath $appIdKey -Name 'DisableProcessIsolation' -Force -ErrorAction SilentlyContinue

        Set-StateValue 'OwnedClsid' $family.Clsid $DllPath
        Set-StateValue 'OwnedAppId' $family.AppId $family.Display

        foreach ($extension in $family.Extensions) {
            $handlerKey = "$ClassesRoot\$extension\shellex\$thumbnailHandlerGuid"
            $existing = Get-RegistryDefaultValue $handlerKey

            if ([string]::IsNullOrEmpty($existing)) {
                Set-RegistryDefaultValue $handlerKey $family.Clsid
                Set-StateValue 'OwnedShellEx' $extension $family.Clsid
                Remove-StateValue 'Conflicts' $extension
                $registered++
            } elseif ($productClsids -contains $existing) {
                # Another (or an older) product CLSID occupies the value: ours to own.
                Set-RegistryDefaultValue $handlerKey $family.Clsid
                Set-StateValue 'OwnedShellEx' $extension $family.Clsid
                Remove-StateValue 'Conflicts' $extension
                $registered++
            } else {
                # Non-product handler: preserve it and record the conflict.
                Set-StateValue 'Conflicts' $extension $existing
                Remove-StateValue 'OwnedShellEx' $extension
                $conflicts += "$extension=$existing"
            }
        }
    }

    Write-Output "registered $registered extension handler(s) for $($families.Count) families"
    if ($conflicts.Count -gt 0) {
        Write-Output ("preserved $($conflicts.Count) pre-existing non-product handler(s): " + ($conflicts -join '; '))
        Write-Output 'interactive Open With support installed; conflicting thumbnail integration was preserved'
    }
}

function Uninstall-ThumbnailRegistration() {
    foreach ($family in $families) {
        foreach ($extension in $family.Extensions) {
            $handlerKey = "$ClassesRoot\$extension\shellex\$thumbnailHandlerGuid"
            $existing = Get-RegistryDefaultValue $handlerKey
            if (-not [string]::IsNullOrEmpty($existing) -and ($productClsids -contains $existing)) {
                # Remove only a value this product owns; a foreign handler survives.
                Remove-RegistryDefaultValue $handlerKey
                if (Test-RegistryKeyEmpty $handlerKey) {
                    Remove-Item -LiteralPath $handlerKey -Force
                }
            }
            Remove-RegistryKeyIfEmpty "$ClassesRoot\$extension\shellex"
            Remove-RegistryKeyIfEmpty "$ClassesRoot\$extension"
        }

        $clsidKey = "$ClassesRoot\CLSID\$($family.Clsid)"
        $ownedDll = Get-StateValue 'OwnedClsid' $family.Clsid
        $inproc = Get-RegistryDefaultValue "$clsidKey\InprocServer32"
        $inprocOwned = [string]::IsNullOrEmpty($inproc) -or
            ($inproc -eq $DllPath) -or
            (-not [string]::IsNullOrEmpty($ownedDll) -and $inproc -eq $ownedDll)
        if ($inprocOwned) {
            if (Test-Path -LiteralPath $clsidKey) {
                Remove-Item -LiteralPath $clsidKey -Recurse -Force
            }
        } else {
            Write-Output "preserved CLSID $($family.Clsid) whose InprocServer32 belongs to another product"
        }

        $appIdKey = "$ClassesRoot\AppID\$($family.AppId)"
        if (Test-Path -LiteralPath $appIdKey) {
            $appIdDefault = Get-RegistryDefaultValue $appIdKey
            if ([string]::IsNullOrEmpty($appIdDefault) -or $appIdDefault -eq $family.Display) {
                Remove-Item -LiteralPath $appIdKey -Recurse -Force
            }
        }
    }

    if (Test-Path -LiteralPath $StateRoot) {
        Remove-Item -LiteralPath $StateRoot -Recurse -Force
    }
    Write-Output "removed product thumbnail registration for $($families.Count) families"
}

switch ($Action) {
    'Install'   { Install-ThumbnailRegistration }
    'Uninstall' { Uninstall-ThumbnailRegistration }
}
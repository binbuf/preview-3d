#requires -Version 7
<#
.SYNOPSIS
    Automated dependency-closure check for Preview3DThumbnailProvider.dll (T05).

.DESCRIPTION
    Fails if the built provider imports any other product binary (the viewer,
    Preview3DImportWorker.exe, either import host, Preview3DOpenUsdCore.dll, ...).
    Those may only ever be reached by the in-proc COM server's static closure,
    never by a runtime import (docs/design/testing-strategy.md, design/05).

    The same boundary is asserted by the `[provider][scaffold]` cases in
    Tests.Unit.exe, which parse the PE import/export tables directly and run as
    part of the repository's baseline verify command. This script is the literal
    `dumpbin /dependents` view used as task evidence; Tests.Unit.exe is the gate.

.EXAMPLE
    pwsh -File tests/unit/check-provider-dependency-closure.ps1 -Configuration Release
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$RepoRoot
)

$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}

$dll = Join-Path $RepoRoot "x64\$Configuration\Preview3DThumbnailProvider.dll"
if (-not (Test-Path -LiteralPath $dll)) {
    throw "Provider DLL not found: $dll. Build it through Preview3D.slnx first."
}

function Find-Dumpbin {
    $command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { return $null }

    $toolsRoot = Join-Path $vs 'VC\Tools\MSVC'
    if (-not (Test-Path -LiteralPath $toolsRoot)) { return $null }
    Get-ChildItem -LiteralPath $toolsRoot -Directory |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\x64\dumpbin.exe' } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
}

$dumpbin = Find-Dumpbin
if (-not $dumpbin) {
    throw 'dumpbin.exe not found. Run from a Visual Studio developer shell.'
}

Write-Output "== dumpbin /dependents $dll =="
$output = & $dumpbin /dependents $dll
$output | ForEach-Object { Write-Output $_ }
if ($LASTEXITCODE -ne 0) { throw "dumpbin failed with exit code $LASTEXITCODE" }

$imports = $output |
    Select-String -Pattern '^\s+(\S+\.(?:dll|exe))\s*$' |
    ForEach-Object { $_.Matches[0].Groups[1].Value }

$forbidden = @($imports | Where-Object { $_ -like 'Preview3D*' })
if ($forbidden.Count -gt 0) {
    throw "Provider imports forbidden product binaries: $($forbidden -join ', ')"
}

Write-Output ""
Write-Output "OK: no viewer/worker/host/core imports ($($imports.Count) imported modules checked)."
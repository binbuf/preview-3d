# T22 developer/QA-local smoke staging (STL only).
#
# Copies the Release x64 Preview3DThumbnailProvider.dll and the non-system
# runtime modules it resolves (the app-local MSVC CRT) into a scratch directory.
# There is no machine-wide install: the registration script points InprocServer32
# at this scratch DLL, and cleanup removes it.
#
# Usage:
#   pwsh -File packaging\smoke\Stage-ProviderSmoke.ps1 -RepositoryRoot <repo>
#
# Outputs the staged directory path; the smoke orchestrator and the registration
# script consume it.

[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$Destination = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$repository = (Resolve-Path -LiteralPath $RepositoryRoot).Path.TrimEnd('\')
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $repository "artifacts\smoke\stage\$Configuration"
}
if (Test-Path -LiteralPath $Destination) {
    Remove-Item -LiteralPath $Destination -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

$providerDll = Join-Path $repository "x64\$Configuration\Preview3DThumbnailProvider.dll"
if (-not (Test-Path -LiteralPath $providerDll -PathType Leaf)) {
    throw "Provider DLL not found: $providerDll. Build Preview3D.slnx $Configuration x64 first."
}

function Find-VisualStudio([string]$Component) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'vswhere.exe was not found; Visual Studio C++ build tools are required.'
    }
    $installation = (& $vswhere -latest -products '*' -requires $Component -property installationPath).Trim()
    if ([string]::IsNullOrWhiteSpace($installation)) {
        throw "No Visual Studio installation with '$Component' was found."
    }
    return $installation
}

function Find-CrtDirectory([string]$VisualStudioRoot) {
    $redistRoot = Join-Path $VisualStudioRoot 'VC\Redist\MSVC'
    $defaultVersionFile = Join-Path $VisualStudioRoot 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.v145.default.txt'
    $versions = @()
    if (Test-Path -LiteralPath $defaultVersionFile -PathType Leaf) {
        $versions += (Get-Content -LiteralPath $defaultVersionFile -Raw).Trim()
    }
    $versions += @(Get-ChildItem -LiteralPath $redistRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { $_.Name })
    foreach ($version in $versions) {
        $x64 = Join-Path $redistRoot "$version\x64"
        $crt = Get-ChildItem -LiteralPath $x64 -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^Microsoft\.VC\d+\.CRT$' } |
            Select-Object -First 1
        if ($null -ne $crt -and (Test-Path -LiteralPath (Join-Path $crt.FullName 'vcruntime140.dll'))) {
            return $crt.FullName
        }
    }
    throw "The x64 app-local MSVC CRT was not found below '$redistRoot'."
}

function Find-Dumpbin([string]$VisualStudioRoot) {
    $toolsRoot = Join-Path $VisualStudioRoot 'VC\Tools\MSVC'
    $dumpbin = Get-ChildItem -LiteralPath $toolsRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\x64\dumpbin.exe' } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not $dumpbin) { throw "dumpbin.exe was not found below '$toolsRoot'." }
    return $dumpbin
}

function Get-PeDependencies([string]$Dumpbin, [string]$Path) {
    $output = & $Dumpbin /nologo /dependents $Path
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed for '$Path'." }
    return @($output | ForEach-Object {
        if ($_ -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') { $matches[1].ToLowerInvariant() }
    } | Sort-Object -Unique)
}

$visualStudio = Find-VisualStudio 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
$crtDirectory = Find-CrtDirectory $visualStudio
$dumpbin = Find-Dumpbin $visualStudio

Copy-Item -LiteralPath $providerDll -Destination (Join-Path $Destination 'Preview3DThumbnailProvider.dll') -Force

# The provider links the VC runtime dynamically; stage the CRT closure app-local
# so the surrogate's loader finds it beside the DLL.
$crtNames = @(
    'concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
    'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'
)
$staged = @('Preview3DThumbnailProvider.dll')
foreach ($name in $crtNames) {
    $source = Join-Path $crtDirectory $name
    if (Test-Path -LiteralPath $source -PathType Leaf) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $Destination $name) -Force
        $staged += $name
    }
}

# Verify the staged closure: every import of every staged binary is a system DLL
# or a staged app-local DLL.
$systemDlls = @(
    'advapi32.dll', 'bcrypt.dll', 'combase.dll', 'comctl32.dll', 'd2d1.dll', 'd3d11.dll', 'd3d12.dll',
    'dbghelp.dll', 'd3dcompiler_47.dll', 'dwrite.dll', 'dwmapi.dll', 'dxgi.dll', 'gdi32.dll',
    'kernel32.dll', 'kernelbase.dll', 'ole32.dll', 'oleaut32.dll', 'powrprof.dll', 'propsys.dll',
    'psapi.dll', 'rpcrt4.dll', 'sechost.dll', 'setupapi.dll', 'shell32.dll', 'shlwapi.dll',
    'ucrtbase.dll', 'user32.dll', 'userenv.dll', 'windowscodecs.dll', 'winmm.dll', 'ws2_32.dll',
    'wsock32.dll', 'xmllite.dll'
)
$stagedLower = @($staged | ForEach-Object { $_.ToLowerInvariant() })
$allStaged = @(Get-ChildItem -LiteralPath $Destination -File | ForEach-Object { $_.Name })
foreach ($name in $allStaged) {
    $path = Join-Path $Destination $name
    foreach ($dependency in (Get-PeDependencies $dumpbin $path)) {
        if ($dependency.StartsWith('api-ms-win-') -or $dependency.StartsWith('ext-ms-win-')) { continue }
        if ($dependency -in $systemDlls) { continue }
        if ($dependency -notin $stagedLower) {
            throw "Unresolved non-system dependency '$dependency' imported by '$name'; add it to the CRT closure."
        }
    }
}

Write-Output "staged $($allStaged.Count) files into $Destination"
foreach ($name in $allStaged) { Write-Output "  $name" }
Write-Output $Destination
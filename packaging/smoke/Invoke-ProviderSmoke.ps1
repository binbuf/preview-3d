# T22 developer/QA-local Release smoke orchestrator (STL only).
#
# Builds, stages, registers, clears the local thumbnail cache, drives a real
# .stl through the Shell's own thumbnail path, then unregisters and cleans up.
# It is the repeatable procedure every later family task re-runs; this is a local
# smoke, NOT the release installer (T41/T42/T44).
#
# Usage:
#   pwsh -File packaging\smoke\Invoke-ProviderSmoke.ps1
#   pwsh -File packaging\smoke\Invoke-ProviderSmoke.ps1 -SkipBuild -KeepRegistered
#
# Exit code 0 means every check passed.

[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$StlPath = '',
    [string]$PlyPath = '',
    [string]$GltfPath = '',
    [string]$FbxPath = '',
    [string]$MfPath = '',
    [string]$UsdPath = '',
    [switch]$SkipBuild,
    [switch]$KeepRegistered,
    [switch]$SkipThumbnailCacheClear
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$repository = (Resolve-Path -LiteralPath $RepositoryRoot).Path.TrimEnd('\')
if ([string]::IsNullOrWhiteSpace($StlPath)) {
    $StlPath = Join-Path $PSScriptRoot 'fixtures\smoke-cube.stl'
}
if ([string]::IsNullOrWhiteSpace($PlyPath)) {
    $PlyPath = Join-Path $PSScriptRoot 'fixtures\smoke-cube.ply'
}
if ([string]::IsNullOrWhiteSpace($GltfPath)) {
    $GltfPath = Join-Path $PSScriptRoot 'fixtures\smoke-cube.glb'
}
if ([string]::IsNullOrWhiteSpace($FbxPath)) {
    $FbxPath = Join-Path $PSScriptRoot 'fixtures\smoke-cube.fbx'
}
if ([string]::IsNullOrWhiteSpace($MfPath)) {
    $MfPath = Join-Path $PSScriptRoot 'fixtures\smoke-cube.3mf'
}
if ([string]::IsNullOrWhiteSpace($UsdPath)) {
    $UsdPath = Join-Path $PSScriptRoot 'fixtures\smoke-cube.usda'
}
if (-not (Test-Path -LiteralPath $StlPath -PathType Leaf)) {
    throw "Smoke .stl not found: $StlPath"
}
if (-not (Test-Path -LiteralPath $PlyPath -PathType Leaf)) {
    throw "Smoke .ply not found: $PlyPath"
}
if (-not (Test-Path -LiteralPath $GltfPath -PathType Leaf)) {
    throw "Smoke .glb not found: $GltfPath"
}
if (-not (Test-Path -LiteralPath $FbxPath -PathType Leaf)) {
    throw "Smoke .fbx not found: $FbxPath"
}
if (-not (Test-Path -LiteralPath $MfPath -PathType Leaf)) {
    throw "Smoke .3mf not found: $MfPath"
}
if (-not (Test-Path -LiteralPath $UsdPath -PathType Leaf)) {
    throw "Smoke .usda not found: $UsdPath"
}

$stage = Join-Path $repository "artifacts\smoke\stage\$Configuration"
$evidence = Join-Path $repository "artifacts\smoke\evidence\$Configuration"
if (Test-Path -LiteralPath $evidence) { Remove-Item -LiteralPath $evidence -Recurse -Force }
New-Item -ItemType Directory -Force -Path $evidence | Out-Null

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'vswhere.exe was not found.' }
    $root = (& $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath).Trim()
    $msbuild = Join-Path $root 'MSBuild\Current\Bin\MSBuild.exe'
    if (-not (Test-Path -LiteralPath $msbuild -PathType Leaf)) { throw "MSBuild.exe not found below '$root'." }
    return $msbuild
}

if (-not $SkipBuild) {
    $msbuild = Find-MSBuild
    $projects = @(
        (Join-Path $repository 'thumbnail-provider\Preview3DThumbnailProvider.vcxproj')
        (Join-Path $repository 'packaging\smoke\ProviderSmokeHost.vcxproj')
    )
    foreach ($project in $projects) {
        & $msbuild $project /t:Build "/p:Configuration=$Configuration" /p:Platform=x64 "/p:SolutionDir=$repository\" /m:1 /v:minimal
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed for $project (exit $LASTEXITCODE). For the full-solution Release build see the procedure note."
        }
    }
}

& (Join-Path $PSScriptRoot 'Stage-ProviderSmoke.ps1') -RepositoryRoot $repository -Configuration $Configuration -Destination $stage | Out-Null
$stagedDll = Join-Path $stage 'Preview3DThumbnailProvider.dll'

if (-not $SkipThumbnailCacheClear) {
    $explorerCache = Join-Path $env:LOCALAPPDATA 'Microsoft\Windows\Explorer'
    foreach ($file in @(Get-ChildItem -LiteralPath $explorerCache -Filter 'thumbcache_*.db' -ErrorAction SilentlyContinue)) {
        try {
            Remove-Item -LiteralPath $file.FullName -Force -ErrorAction Stop
            Write-Output "cleared thumbnail cache $($file.Name)"
        } catch {
            Write-Output "thumbnail cache $($file.Name) locked; relying on WTS_FORCEEXTRACTION"
        }
    }
    if (Test-Path "$env:SystemRoot\System32\ie4uinit.exe") {
        & "$env:SystemRoot\System32\ie4uinit.exe" -ClearIconCache 2>$null | Out-Null
    }
}

& (Join-Path $PSScriptRoot 'Register-ProviderSmoke.ps1') -DllPath $stagedDll -Scope HKCU
$hostExe = Join-Path $repository "x64\$Configuration\ProviderSmokeHost.exe"
$stlExit = 1
$plyExit = 1
$gltfExit = 1
$fbxExit = 1
$mfExit = 1
$usdExit = 1
try {
    & $hostExe --dll $stagedDll --stl $StlPath --cx 256 --out $evidence
    $stlExit = $LASTEXITCODE

    & $hostExe --dll $stagedDll --ply $PlyPath --cx 256 --out $evidence
    $plyExit = $LASTEXITCODE

    & $hostExe --dll $stagedDll --gltf $GltfPath --cx 256 --out $evidence
    $gltfExit = $LASTEXITCODE

    & $hostExe --dll $stagedDll --fbx $FbxPath --cx 256 --out $evidence
    $fbxExit = $LASTEXITCODE

    & $hostExe --dll $stagedDll --mf $MfPath --cx 256 --out $evidence
    $mfExit = $LASTEXITCODE

    & $hostExe --dll $stagedDll --usd $UsdPath --cx 256 --out $evidence
    $usdExit = $LASTEXITCODE
} finally {
    if (-not $KeepRegistered) {
        & (Join-Path $PSScriptRoot 'Unregister-ProviderSmoke.ps1') -Scope HKCU
    } else {
        Write-Warning "registration left in place (HKCU); run Unregister-ProviderSmoke.ps1 to remove it"
    }
}

Write-Output "evidence directory: $evidence"
if ($stlExit -ne 0 -or $plyExit -ne 0 -or $gltfExit -ne 0 -or $fbxExit -ne 0 -or $mfExit -ne 0 -or $usdExit -ne 0) {
    Write-Output "smoke FAILED (stl exit $stlExit, ply exit $plyExit, gltf exit $gltfExit, fbx exit $fbxExit, 3mf exit $mfExit, usd exit $usdExit)"
    exit 1
}
exit 0
[CmdletBinding()]
param(
    [string]$RepositoryRoot = '',

    [string]$CertificateThumbprint = '',

    [string]$TimestampUrl = 'https://timestamp.digicert.com',

    [ValidatePattern('^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$')]
    [string]$Version = '0.3.6',

    [string]$NsisPath = '',

    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

function Resolve-FullPath([string]$Path) {
    return [System.IO.Path]::GetFullPath($Path)
}

function Assert-ChildPath([string]$Parent, [string]$Child) {
    $parentFull = (Resolve-FullPath $Parent).TrimEnd('\') + '\'
    $childFull = Resolve-FullPath $Child
    if (-not $childFull.StartsWith($parentFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside '$parentFull': '$childFull'."
    }
}

function Find-Nsis([string]$RequestedPath) {
    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        if (-not (Test-Path -LiteralPath $RequestedPath -PathType Leaf)) {
            throw "NsisPath does not identify makensis.exe: $RequestedPath"
        }
        return (Resolve-FullPath $RequestedPath)
    }

    $command = Get-Command makensis.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) { return $command.Source }

    $programFilesX86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
    foreach ($candidate in @(
        (Join-Path $programFilesX86 'NSIS\makensis.exe'),
        (Join-Path $programFilesX86 'NSIS\Bin\makensis.exe')
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    throw 'NSIS 3 was not found. Install NSIS or pass /p:NsisPath=<path-to-makensis.exe>.'
}

function Find-MSBuild {
    $programFilesX86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
    $vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'vswhere.exe was not found; Visual Studio C++ build tools are required.'
    }
    $visualStudioRoot = (& $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath).Trim()
    if ([string]::IsNullOrWhiteSpace($visualStudioRoot)) {
        throw 'No Visual Studio installation with MSBuild was found.'
    }
    $msbuild = Join-Path $visualStudioRoot 'MSBuild\Current\Bin\MSBuild.exe'
    if (-not (Test-Path -LiteralPath $msbuild -PathType Leaf)) {
        throw "MSBuild.exe was not found below '$visualStudioRoot'."
    }
    return $msbuild
}

function Find-SignTool {
    $programFilesX86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
    $windowsKits = Join-Path $programFilesX86 'Windows Kits\10\bin'
    $candidate = Get-ChildItem -LiteralPath $windowsKits -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
        Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
    if ([string]::IsNullOrWhiteSpace($candidate)) {
        throw 'An installer signing thumbprint was supplied, but x64 signtool.exe could not be found.'
    }
    return $candidate
}

function Get-Sha256([string]$Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha.ComputeHash($stream)
        return ([System.BitConverter]::ToString($bytes)).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
        $stream.Dispose()
    }
}

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot '..\..'
}
$repository = (Resolve-FullPath $RepositoryRoot).TrimEnd('\')
$artifacts = Join-Path $repository 'artifacts\installer'
$stage = Join-Path $artifacts 'stage'
$installer = Join-Path $artifacts "Preview3D-$Version-x64-setup.exe"
$installerChecksum = "$installer.sha256"
$payloadScript = Join-Path $repository 'packaging\portable\Create-PortableRelease.ps1'
$nsisScript = Join-Path $repository 'packaging\installer\Preview3D.nsi'

Assert-ChildPath $repository $artifacts
Assert-ChildPath $artifacts $stage
New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
foreach ($path in @($installer, $installerChecksum)) {
    if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
}

if (-not $SkipBuild) {
    $msbuild = Find-MSBuild
    # The viewer project has build-order references to both isolated import
    # executables and the OpenUSD host's private core payload.
    $viewerProject = Join-Path $repository 'interactive-viewer\Preview3D.vcxproj'
    & $msbuild $viewerProject /t:Build /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=$repository\" /m:1 /v:minimal
    if ($LASTEXITCODE -ne 0) {
        throw "The Release x64 product build failed with exit code $LASTEXITCODE."
    }
}

& $payloadScript -RepositoryRoot $repository -CertificateThumbprint $CertificateThumbprint `
    -TimestampUrl $TimestampUrl -Version $Version -Distribution Installer

$numericVersion = ($Version -split '[-+]')[0]
$numericParts = @($numericVersion -split '\.' | ForEach-Object { [int]$_ })
if ($numericParts | Where-Object { $_ -gt 65535 }) {
    throw "Version '$Version' contains a component larger than NSIS supports (65535)."
}
$fileVersion = "$numericVersion.0"

$makensis = Find-Nsis $NsisPath
$nsisArguments = @(
    '/V3',
    '/WX',
    "/DSTAGE_DIR=$stage",
    "/DOUTPUT_FILE=$installer",
    "/DPRODUCT_VERSION=$Version",
    "/DPRODUCT_FILE_VERSION=$fileVersion"
)
$signTool = ''
if (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
    $signTool = Find-SignTool
    $nsisArguments += "/DSIGNTOOL=$signTool"
    $nsisArguments += "/DSIGN_THUMBPRINT=$CertificateThumbprint"
    $nsisArguments += "/DTIMESTAMP_URL=$TimestampUrl"
}
$nsisArguments += $nsisScript

& $makensis @nsisArguments
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $installer -PathType Leaf)) {
    throw "makensis failed with exit code $LASTEXITCODE."
}

if (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
    & $signTool sign /sha1 $CertificateThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 $installer
    if ($LASTEXITCODE -ne 0) { throw "Signing failed for '$installer'." }
    & $signTool verify /pa /all $installer
    if ($LASTEXITCODE -ne 0) { throw "Signature verification failed for '$installer'." }
} else {
    Write-Warning 'Creating an unsigned engineering installer. A signed setup is required for release acceptance.'
}

$installerHash = Get-Sha256 $installer
"$installerHash  $([System.IO.Path]::GetFileName($installer))" |
    Set-Content -LiteralPath $installerChecksum -Encoding ASCII

Write-Host "Installer: $installer"
Write-Host "SHA-256: $installerHash"

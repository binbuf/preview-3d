<#
.SYNOPSIS
    Release-metadata gate for SEC-19 / T19.

.DESCRIPTION
    Validates that the shipped SBOM/license metadata is generated from (and
    covers) the installed vcpkg dependency closure, and that the release version
    cannot drift between vcpkg.json, Directory.Solution.targets, and
    Preview3D.nsi. The packaging scripts call the same ReleaseMetadata.ps1
    functions, so a failure here is the same failure that stops a build.

    -SelfTest additionally proves the negative cases: a closure package with no
    license mapping must fail, and a version mismatch must fail.

.EXAMPLE
    pwsh -File packaging/Test-ReleaseMetadata.ps1 -SelfTest
#>
[CmdletBinding()]
param(
    [string]$RepositoryRoot = '',
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\')

. (Join-Path $PSScriptRoot 'ReleaseMetadata.ps1')

$failures = [System.Collections.Generic.List[string]]::new()

function Assert-ExpectedFailure([string]$Name, [scriptblock]$Action) {
    try {
        & $Action
        $failures.Add("$Name did not fail as required")
    } catch {
        Write-Host "OK (expected failure): $Name"
    }
}

if ($SelfTest) {
    $unmapped = [ordered]@{
        '__definitely-not-mapped__' = [ordered]@{
            name = '__definitely-not-mapped__'; version = '1.0'; abi = $null
            copyrightPath = 'does-not-exist'; tree = 'synthetic'
        }
    }
    Assert-ExpectedFailure 'unmapped installed dependency' {
        Assert-LicenseMappingCoversClosure $unmapped @{ basisu = @{ spdx = 'Apache-2.0' } }
    }

    $temp = Join-Path ([System.IO.Path]::GetTempPath()) ("preview3d-release-metadata-" + [guid]::NewGuid().ToString('n'))
    New-Item -ItemType Directory -Path (Join-Path $temp 'packaging\installer') -Force | Out-Null
    try {
        '{ "name": "preview3d", "version": "9.9.9" }' | Set-Content -LiteralPath (Join-Path $temp 'vcpkg.json') -Encoding UTF8
        Copy-Item -LiteralPath (Join-Path $RepositoryRoot 'Directory.Solution.targets') -Destination $temp -Force
        Copy-Item -LiteralPath (Join-Path $RepositoryRoot 'packaging\installer\Preview3D.nsi') -Destination (Join-Path $temp 'packaging\installer') -Force
        Assert-ExpectedFailure 'release version drift' {
            Assert-ReleaseVersionConsistency -RepositoryRoot $temp | Out-Null
        }
    } finally {
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    }

    if ($failures.Count -ne 0) {
        throw "Release-metadata self-test failure(s): $($failures -join '; ')"
    }
}

$version = Assert-ReleaseVersionConsistency -RepositoryRoot $RepositoryRoot
Write-Host "OK: release version consistently $version"

$mapping = Import-DependencyLicenseMapping -RepositoryRoot $RepositoryRoot
Write-Host "OK: license mapping loaded ($($mapping.Count) entries)"

Assert-LicensesDocCoversMapping -RepositoryRoot $RepositoryRoot -Mapping $mapping
Write-Host 'OK: THIRD-PARTY-LICENSES.md covers every mapped package and links the baseline'

$installedTrees = @(Get-VcpkgInstallTree -RepositoryRoot $RepositoryRoot |
    Where-Object { Test-Path -LiteralPath $_.StatusPath -PathType Leaf })
if ($installedTrees.Count -eq 0) {
    Write-Host 'SKIP: no installed vcpkg tree present; closure/copyright checks not run'
    exit 0
}

$closure = Get-InstalledDependencyClosure -RepositoryRoot $RepositoryRoot
Assert-ManifestRootsPresent -RepositoryRoot $RepositoryRoot -Closure $closure
Assert-LicenseMappingCoversClosure -Closure $closure -Mapping $mapping
Assert-CopyrightFilesExist -Closure $closure
Write-Host "OK: installed closure ($($closure.Count) packages) is fully mapped with copyright files"

exit 0
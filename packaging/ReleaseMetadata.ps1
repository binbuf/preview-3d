<#
Shared release-metadata logic for SEC-19 / T19.

This module is dot-sourced by packaging/portable/Create-PortableRelease.ps1 and
packaging/installer/Create-Installer.ps1 and exercised by
packaging/Test-ReleaseMetadata.ps1. It is the single definition of:

  * how an installed vcpkg tree is parsed into a dependency closure;
  * the SPDX license mapping gate (an installed package with no mapping fails);
  * the release-version consistency gate (vcpkg.json, Directory.Solution.targets,
    Preview3D.nsi, and the packaging -Version argument must agree).

Design note from the task: "Prefer generation over manual lists everywhere; a
manual list that cannot fail is a defect."
#>

Set-StrictMode -Version 3.0

$script:TargetTriplet = 'x64-windows-static-md'
$script:LicenseMappingRelativePath = 'packaging\portable\dependency-licenses.json'

# Parses a vcpkg `status` file (Debian-control-style stanzas) into a map of
# package name -> field map. Duplicate stanzas (feature records) are ignored.
function Read-VcpkgStatus([string]$StatusPath) {
    $result = @{}
    $current = @{}
    foreach ($line in (Get-Content -LiteralPath $StatusPath)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            if ($current.ContainsKey('Package') -and -not $result.ContainsKey($current.Package)) {
                $result[$current.Package] = $current
            }
            $current = @{}
            continue
        }
        if ($line -match '^([^:]+):\s*(.*)$') {
            $current[$matches[1]] = $matches[2]
        }
    }
    if ($current.ContainsKey('Package') -and -not $result.ContainsKey($current.Package)) {
        $result[$current.Package] = $current
    }
    return $result
}

function Get-VcpkgPackageVersion([System.Collections.IDictionary]$Entry) {
    $version = if ($Entry.Contains('Version')) { $Entry.Version }
        elseif ($Entry.Contains('Version-Semver')) { $Entry.'Version-Semver' }
        else { 'unknown' }
    if ($Entry.Contains('Port-Version') -and $Entry.'Port-Version' -ne '0') {
        $version = "$version#$($Entry.'Port-Version')"
    }
    return $version
}

# The vcpkg trees whose licenses/metadata the shipped payload can draw from:
# the root manifest (viewer/worker/OpenUSD host) and each isolated STEP tree
# (dedicated STEP host and thumbnail provider). A missing tree is tolerated
# here; Get-InstalledDependencyClosure decides whether that is fatal.
function Get-VcpkgInstallTree {
    param([string]$RepositoryRoot)

    $trees = @(
        @{
            Name       = 'root'
            StatusPath = Join-Path $RepositoryRoot 'vcpkg_installed\x64-windows-static-md\vcpkg\status'
            ShareDir   = Join-Path $RepositoryRoot 'vcpkg_installed\x64-windows-static-md\x64-windows-static-md\share'
        },
        @{
            Name       = 'step-host'
            StatusPath = Join-Path $RepositoryRoot 'compatibility-host-step\vcpkg_installed\vcpkg\status'
            ShareDir   = Join-Path $RepositoryRoot 'compatibility-host-step\vcpkg_installed\x64-windows-static-md\share'
        },
        @{
            Name       = 'step-provider'
            StatusPath = Join-Path $RepositoryRoot 'thumbnail-provider\step-occt\vcpkg_installed\vcpkg\status'
            ShareDir   = Join-Path $RepositoryRoot 'thumbnail-provider\step-occt\vcpkg_installed\x64-windows-static-md\share'
        }
    )
    return $trees
}

# Returns the installed target-architecture dependency closure across every
# present vcpkg tree, keyed by package name. Host-only build tools (installed
# for x64-windows) are excluded; the closure is therefore exactly the manifest
# dependencies plus their transitives, never a hand-maintained list.
function Get-InstalledDependencyClosure {
    param([string]$RepositoryRoot)

    $closure = [ordered]@{}
    foreach ($tree in (Get-VcpkgInstallTree $RepositoryRoot)) {
        if (-not (Test-Path -LiteralPath $tree.StatusPath -PathType Leaf)) { continue }
        $status = Read-VcpkgStatus $tree.StatusPath
        foreach ($name in $status.Keys) {
            $entry = $status[$name]
            if ($entry.Architecture -ne $script:TargetTriplet) { continue }
            if ($closure.Contains($name)) { continue }
            $closure[$name] = [ordered]@{
                name          = $name
                version       = Get-VcpkgPackageVersion $entry
                abi           = if ($entry.Contains('Abi')) { $entry.Abi } else { $null }
                copyrightPath = Join-Path $tree.ShareDir "$name\copyright"
                tree          = $tree.Name
            }
        }
    }
    return $closure
}

function Get-ManifestRootDependencies {
    param([string]$RepositoryRoot)
    $manifest = Get-Content -LiteralPath (Join-Path $RepositoryRoot 'vcpkg.json') -Raw | ConvertFrom-Json
    return @($manifest.dependencies | ForEach-Object {
        if ($_ -is [string]) { $_ } else { $_.name }
    } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)
}

# The generated set must be the manifest roots plus their transitives. Every
# declared root must be installed (and therefore present in the closure); if
# one is not, the manifest and the installed tree disagree.
function Assert-ManifestRootsPresent {
    param([string]$RepositoryRoot, [System.Collections.IDictionary]$Closure)

    $missing = @(Get-ManifestRootDependencies -RepositoryRoot $RepositoryRoot |
        Where-Object { -not $Closure.Contains($_) } | Sort-Object)
    if ($missing.Count -ne 0) {
        throw "vcpkg.json declares root dependencies that are not installed: $($missing -join ', '). Restore the manifest before packaging."
    }
}

function Import-DependencyLicenseMapping {
    param([string]$RepositoryRoot)

    $path = Join-Path $RepositoryRoot $script:LicenseMappingRelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "The dependency license mapping is missing: $path"
    }
    # Windows PowerShell 5.1 (the MSBuild packaging interpreter) has no
    # ConvertFrom-Json -AsHashtable, so walk the PSObject properties instead.
    $parsed = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    $mapping = @{}
    foreach ($property in $parsed.PSObject.Properties) {
        if ($property.Name -like '$*') { continue }
        $mapping[$property.Name] = @{ spdx = $property.Value.spdx }
    }
    return $mapping
}

# Fail closed: every installed target dependency must have a reviewed SPDX
# mapping, so adding a vcpkg port cannot silently omit its license.
function Assert-LicenseMappingCoversClosure {
    param(
        [System.Collections.IDictionary]$Closure,
        [System.Collections.IDictionary]$Mapping
    )

    $missing = @($Closure.Keys | Where-Object { -not $Mapping.Contains($_) } | Sort-Object)
    if ($missing.Count -ne 0) {
        throw ("Installed dependency/dependencies have no license mapping in " +
            "$($script:LicenseMappingRelativePath): $($missing -join ', '). Review the upstream " +
            'license and add an SPDX expression before packaging.')
    }
}

function Assert-CopyrightFilesExist {
    param([System.Collections.IDictionary]$Closure)

    $missing = @($Closure.Keys | Where-Object {
        -not (Test-Path -LiteralPath $Closure[$_]['copyrightPath'] -PathType Leaf)
    } | Sort-Object)
    if ($missing.Count -ne 0) {
        throw "Installed dependency/dependencies have no upstream copyright file installed: $($missing -join ', ')."
    }
}

function Get-ManifestVersion {
    param([string]$RepositoryRoot)
    $path = Join-Path $RepositoryRoot 'vcpkg.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing manifest: $path" }
    return (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json).version
}

function Get-TargetsReleaseVersion {
    param([string]$RepositoryRoot)
    $path = Join-Path $RepositoryRoot 'Directory.Solution.targets'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing targets file: $path" }
    $text = Get-Content -LiteralPath $path -Raw
    if ($text -notmatch '<ReleaseVersion[^>]*>([^<]+)</ReleaseVersion>') {
        throw "Directory.Solution.targets has no <ReleaseVersion> default to check."
    }
    return $matches[1].Trim()
}

function Get-InstallerDefaultVersion {
    param([string]$RepositoryRoot)
    $path = Join-Path $RepositoryRoot 'packaging\installer\Preview3D.nsi'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing installer script: $path" }
    $text = Get-Content -LiteralPath $path -Raw
    if ($text -notmatch '!define\s+PRODUCT_VERSION\s+"([^"]+)"') {
        throw "Preview3D.nsi has no PRODUCT_VERSION default to check."
    }
    return $matches[1]
}

function Get-ReleaseVersionSources {
    param([string]$RepositoryRoot, [string]$ExpectedVersion = '')

    $versions = [ordered]@{
        'vcpkg.json'                          = Get-ManifestVersion $RepositoryRoot
        'Directory.Solution.targets'          = Get-TargetsReleaseVersion $RepositoryRoot
        'packaging/installer/Preview3D.nsi'   = Get-InstallerDefaultVersion $RepositoryRoot
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedVersion)) {
        $versions['packaging -Version'] = $ExpectedVersion
    }
    return $versions
}

# The version cannot drift between the dependency manifest, the solution-level
# release default, the installer's fallback, and the version passed to the
# packaging script.
function Assert-ReleaseVersionConsistency {
    param([string]$RepositoryRoot, [string]$ExpectedVersion = '')

    $versions = Get-ReleaseVersionSources -RepositoryRoot $RepositoryRoot -ExpectedVersion $ExpectedVersion
    $distinct = @($versions.Values | Sort-Object -Unique)
    if ($distinct.Count -ne 1) {
        $detail = (($versions.GetEnumerator() | ForEach-Object { "  $($_.Key) = $($_.Value)" }) -join [Environment]::NewLine)
        throw "Release version drift detected; these must all match:$([Environment]::NewLine)$detail"
    }
    return $distinct[0]
}

# The committed navigation doc must name every mapped package and link the
# pinned vcpkg baseline, so removing a component (or the baseline) fails the
# gate rather than silently drifting.
function Assert-LicensesDocCoversMapping {
    param([string]$RepositoryRoot, [System.Collections.IDictionary]$Mapping)

    $path = Join-Path $RepositoryRoot 'THIRD-PARTY-LICENSES.md'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing licenses doc: $path" }
    $text = Get-Content -LiteralPath $path -Raw

    $missing = @($Mapping.Keys | Where-Object { $text -notmatch [regex]::Escape($_) } | Sort-Object)
    if ($missing.Count -ne 0) {
        throw "THIRD-PARTY-LICENSES.md omits mapped package(s): $($missing -join ', ')."
    }

    $configPath = Join-Path $RepositoryRoot 'vcpkg-configuration.json'
    $baseline = (Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json).'default-registry'.baseline
    if ($text -notmatch [regex]::Escape($baseline)) {
        throw "THIRD-PARTY-LICENSES.md does not link the pinned vcpkg baseline '$baseline'."
    }
}
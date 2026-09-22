[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,

    [string]$CertificateThumbprint = '',

    [string]$TimestampUrl = 'https://timestamp.digicert.com',

    [ValidatePattern('^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$')]
    [string]$Version = '0.3.6',

    [ValidateSet('Portable', 'Installer')]
    [string]$Distribution = 'Portable'
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

function Copy-RequiredFile([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Required release input is missing: $Source"
    }
    $destinationDirectory = Split-Path -Parent $Destination
    if (-not (Test-Path -LiteralPath $destinationDirectory -PathType Container)) {
        New-Item -ItemType Directory -Path $destinationDirectory | Out-Null
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Find-VisualStudioInstallation {
    $programFilesX86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
    $vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'vswhere.exe was not found; Visual Studio C++ build tools are required to package the app-local CRT.'
    }
    $installation = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if ([string]::IsNullOrWhiteSpace($installation)) {
        throw 'No Visual Studio installation with the x64 C++ tools was found.'
    }
    return $installation
}

function Find-CrtDirectory([string]$VisualStudioRoot) {
    $redistRoot = Join-Path $VisualStudioRoot 'VC\Redist\MSVC'
    $defaultVersionFile = Join-Path $VisualStudioRoot 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.v145.default.txt'
    if (Test-Path -LiteralPath $defaultVersionFile -PathType Leaf) {
        $defaultVersion = (Get-Content -LiteralPath $defaultVersionFile -Raw).Trim()
        $defaultX64 = Join-Path $redistRoot "$defaultVersion\x64"
        $defaultCrt = Get-ChildItem -LiteralPath $defaultX64 -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^Microsoft\.VC\d+\.CRT$' } |
            Select-Object -First 1
        if ($null -ne $defaultCrt -and (Test-Path -LiteralPath (Join-Path $defaultCrt.FullName 'vcruntime140.dll'))) {
            return $defaultCrt.FullName
        }
        throw "The v145 default toolset is '$defaultVersion', but its x64 app-local CRT was not found below '$defaultX64'."
    }
    $candidates = Get-ChildItem -LiteralPath $redistRoot -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending
    foreach ($candidate in $candidates) {
        $crt = Get-ChildItem -LiteralPath (Join-Path $candidate.FullName 'x64') -Directory -ErrorAction SilentlyContinue |
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
    $defaultVersionFile = Join-Path $VisualStudioRoot 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.v145.default.txt'
    if (Test-Path -LiteralPath $defaultVersionFile -PathType Leaf) {
        $defaultVersion = (Get-Content -LiteralPath $defaultVersionFile -Raw).Trim()
        $defaultDumpbin = Join-Path $toolsRoot "$defaultVersion\bin\Hostx64\x64\dumpbin.exe"
        if (Test-Path -LiteralPath $defaultDumpbin -PathType Leaf) {
            return $defaultDumpbin
        }
        throw "The v145 default toolset is '$defaultVersion', but dumpbin.exe was not found at '$defaultDumpbin'."
    }
    $tools = Get-ChildItem -LiteralPath $toolsRoot -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending
    foreach ($tool in $tools) {
        $candidate = Join-Path $tool.FullName 'bin\Hostx64\x64\dumpbin.exe'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    throw "dumpbin.exe was not found below '$toolsRoot'."
}

function Get-PeDependencies([string]$Dumpbin, [string]$Path) {
    $output = & $Dumpbin /nologo /dependents $Path
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed for '$Path'."
    }
    return @($output | ForEach-Object {
        if ($_ -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') {
            $matches[1].ToLowerInvariant()
        }
    } | Sort-Object -Unique)
}

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

$repository = (Resolve-FullPath $RepositoryRoot).TrimEnd('\')
$buildOutput = Join-Path $repository 'x64\Release'
$distributionDirectory = $Distribution.ToLowerInvariant()
$artifacts = Join-Path $repository "artifacts\$distributionDirectory"
$stage = Join-Path $artifacts 'stage'
$archive = Join-Path $artifacts "Preview3D-$Version-portable-x64.zip"
$archiveChecksum = "$archive.sha256"
$workerStage = Join-Path $stage 'worker'
$openUsdHostStage = Join-Path $stage 'OpenUsdHost'
$stepHostStage = Join-Path $stage 'StepHost'
$licensesStage = Join-Path $stage 'licenses'

Assert-ChildPath $repository $artifacts
Assert-ChildPath $artifacts $stage
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $workerStage -Force | Out-Null
New-Item -ItemType Directory -Path $openUsdHostStage -Force | Out-Null
New-Item -ItemType Directory -Path $stepHostStage -Force | Out-Null
New-Item -ItemType Directory -Path $licensesStage -Force | Out-Null
if ($Distribution -eq 'Portable' -and (Test-Path -LiteralPath $archive)) {
    Remove-Item -LiteralPath $archive -Force
}
if ($Distribution -eq 'Portable' -and (Test-Path -LiteralPath $archiveChecksum)) {
    Remove-Item -LiteralPath $archiveChecksum -Force
}

$viewerFiles = @('Preview3D.exe')
# The release triplet statically links the worker's dependency closure, so no
# upstream DLLs (draco, fastgltf, ktx, lib3mf, libwebp, meshoptimizer, simdjson,
# zlib, libzip, bzip2, zstd) are deployed or shipped. Only the app's own images
# remain, which keeps Smart App Control and code-signing coverage tractable.
$workerFiles = @(
    'Preview3DImportWorker.exe'
)
foreach ($name in $viewerFiles) {
    Copy-RequiredFile (Join-Path $buildOutput $name) (Join-Path $stage $name)
}
foreach ($name in $workerFiles) {
    Copy-RequiredFile (Join-Path $buildOutput $name) (Join-Path $workerStage $name)
}

$openUsdHostSource = Join-Path $buildOutput 'OpenUsdHost'
# OpenUSD, TBB, KTX, libwebp, and zstd are statically linked into
# Preview3DOpenUsdCore.dll by the release triplet. Only the host executable, the
# app's own core DLL, and OpenUSD's generated schema/plugin resources ship.
$openUsdHostFiles = @(
    'Preview3DImportHost.exe',
    'Preview3DOpenUsdCore.dll',
    'usd\plugInfo.json',
    'usd\ar\resources\plugInfo.json',
    'usd\preview3d\resources\plugInfo.json',
    'usd\sdf\resources\plugInfo.json',
    'usd\usd\resources\generatedSchema.usda',
    'usd\usd\resources\plugInfo.json',
    'usd\usd\resources\usd\schema.usda',
    'usd\usdGeom\resources\generatedSchema.usda',
    'usd\usdGeom\resources\plugInfo.json',
    'usd\usdGeom\resources\usdGeom\schema.usda',
    'usd\usdShade\resources\generatedSchema.usda',
    'usd\usdShade\resources\plugInfo.json',
    'usd\usdShade\resources\usdShade\schema.usda'
)
foreach ($relativePath in $openUsdHostFiles) {
    Copy-RequiredFile (Join-Path $openUsdHostSource $relativePath) (Join-Path $openUsdHostStage $relativePath)
}

$stepHostSource = Join-Path $buildOutput 'StepHost'
# The release triplet statically links the constrained OCCT closure into the
# dedicated STEP host, so the modeling/data-exchange/foundation toolkits are
# not shipped as separate DLLs. Visualization, Draw, DETools, and non-STEP
# exchange toolkits remain deliberately absent from the port.
$stepHostFiles = @(
    'Preview3DStepHost.exe'
)
foreach ($name in $stepHostFiles) {
    Copy-RequiredFile (Join-Path $stepHostSource $name) (Join-Path $stepHostStage $name)
}

$visualStudioRoot = Find-VisualStudioInstallation
$crtDirectory = Find-CrtDirectory $visualStudioRoot
$viewerCrt = @('concrt140.dll', 'msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
$workerCrt = @('concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
$openUsdHostCrt = @('concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
$stepHostCrt = @('concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
foreach ($name in $viewerCrt) {
    Copy-RequiredFile (Join-Path $crtDirectory $name) (Join-Path $stage $name)
}
foreach ($name in $workerCrt) {
    Copy-RequiredFile (Join-Path $crtDirectory $name) (Join-Path $workerStage $name)
}
foreach ($name in $openUsdHostCrt) {
    Copy-RequiredFile (Join-Path $crtDirectory $name) (Join-Path $openUsdHostStage $name)
}
foreach ($name in $stepHostCrt) {
    Copy-RequiredFile (Join-Path $crtDirectory $name) (Join-Path $stepHostStage $name)
}

$thirdParty = @('basisu', 'bzip2', 'draco', 'fastgltf', 'ktx', 'lib3mf', 'libwebp', 'libzip', 'meshoptimizer', 'opencascade', 'openusd', 'simdjson', 'tbb', 'tinyusdz', 'ufbx', 'zlib', 'zstd')
$vcpkgTripletRoot = Join-Path $repository 'vcpkg_installed\x64-windows-static-md\x64-windows-static-md'
$vcpkgStatusPath = Join-Path $repository 'vcpkg_installed\x64-windows-static-md\vcpkg\status'
# OCCT is deliberately installed only for the dedicated STEP host, so its
# license and metadata come from that manifest's separate vcpkg tree.
$stepVcpkgRoot = Join-Path $repository 'compatibility-host-step\vcpkg_installed\x64-windows-static-md'
foreach ($name in $thirdParty) {
    if ($name -eq 'opencascade') {
        Copy-RequiredFile (Join-Path $stepVcpkgRoot 'share\opencascade\copyright') (Join-Path $licensesStage "$name.txt")
    } else {
        Copy-RequiredFile (Join-Path $vcpkgTripletRoot "share\$name\copyright") (Join-Path $licensesStage "$name.txt")
    }
}
Copy-RequiredFile (Join-Path $repository 'LICENSE') (Join-Path $stage 'LICENSE')
Copy-RequiredFile (Join-Path $repository 'NOTICE') (Join-Path $stage 'NOTICE')
Copy-RequiredFile (Join-Path $repository 'packaging\portable\THIRD-PARTY-NOTICES.txt') (Join-Path $stage 'THIRD-PARTY-NOTICES.txt')
if ($Distribution -eq 'Portable') {
    Copy-RequiredFile (Join-Path $repository 'packaging\portable\PORTABLE-README.txt') (Join-Path $stage 'README.txt')
    Copy-RequiredFile (Join-Path $repository 'packaging\portable\Remove-Preview3DProfile.ps1') (Join-Path $stage 'Remove-Preview3DProfile.ps1')
} else {
    Copy-RequiredFile (Join-Path $repository 'packaging\installer\INSTALLER-README.txt') (Join-Path $stage 'README.txt')
    Copy-RequiredFile (Join-Path $repository 'packaging\portable\Remove-Preview3DProfile.ps1') (Join-Path $stage 'Remove-Preview3DProfile.ps1')
    Copy-RequiredFile (Join-Path $repository 'packaging\installer\Provision-Preview3DWorkerAcl.ps1') (Join-Path $stage 'Provision-Preview3DWorkerAcl.ps1')
}
$stagedReadme = Join-Path $stage 'README.txt'
(Get-Content -LiteralPath $stagedReadme -Raw).Replace('@VERSION@', $Version) |
    Set-Content -LiteralPath $stagedReadme -Encoding UTF8

$signed = $false
if (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
    $signTool = Get-ChildItem -LiteralPath (Join-Path $visualStudioRoot 'Common7\Tools') -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if ([string]::IsNullOrWhiteSpace($signTool)) {
        $windowsKits = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)) 'Windows Kits\10\bin'
        $signTool = Get-ChildItem -LiteralPath $windowsKits -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1 -ExpandProperty FullName
    }
    if ([string]::IsNullOrWhiteSpace($signTool)) {
        throw 'A signing thumbprint was supplied, but signtool.exe could not be found.'
    }
    # Sign every executable and DLL in the payload, not just the product
    # binaries. Windows Smart App Control and WDAC evaluate each image
    # individually: an unsigned third-party dependency (zstd.dll, draco.dll,
    # usd_ms.dll, the OCCT TK*.dll closure) is refused with the Bad Image status
    # 0xC0E90002 even when the viewer/worker that load it are signed. Images
    # that already carry a trusted signature (the Microsoft CRT) are skipped.
    $payloadBinaries = @(Get-ChildItem -LiteralPath $stage -File -Recurse |
        Where-Object { $_.Extension -in @('.exe', '.dll') })
    foreach ($binary in $payloadBinaries) {
        & $signTool verify /pa /q $binary.FullName 2>$null
        if ($LASTEXITCODE -eq 0) { continue }
        & $signTool sign /sha1 $CertificateThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 $binary.FullName
        if ($LASTEXITCODE -ne 0) { throw "Signing failed for '$($binary.FullName)'." }
        & $signTool verify /pa /all $binary.FullName
        if ($LASTEXITCODE -ne 0) { throw "Signature verification failed for '$($binary.FullName)'." }
    }
    $signed = $true
} else {
    Write-Warning "Creating an unsigned engineering $distributionDirectory payload. Release acceptance requires an Authenticode certificate thumbprint."
}

$dumpbin = Find-Dumpbin $visualStudioRoot
$systemDlls = @(
    'advapi32.dll', 'bcrypt.dll', 'comctl32.dll', 'd2d1.dll', 'd3d11.dll', 'd3d12.dll', 'dbghelp.dll',
    'd3dcompiler_47.dll', 'dwrite.dll', 'dwmapi.dll', 'dxgi.dll', 'gdi32.dll',
    'kernel32.dll', 'ole32.dll', 'oleacc.dll', 'oleaut32.dll', 'runtimeobject.dll',
    'shell32.dll', 'shlwapi.dll', 'uiautomationcore.dll', 'user32.dll', 'userenv.dll',
    'windowscodecs.dll', 'winmm.dll', 'ws2_32.dll', 'wsock32.dll', 'xmllite.dll'
)
$peFiles = Get-ChildItem -LiteralPath $stage -File -Recurse | Where-Object { $_.Extension -in @('.exe', '.dll') }
foreach ($pe in $peFiles) {
    # Windows does not search a sibling/parent payload directory. Validate
    # each binary against DLLs colocated with that binary, matching the
    # actual app-local loader behavior for both viewer and worker.
    $localDllNames = @(Get-ChildItem -LiteralPath $pe.DirectoryName -Filter *.dll -File |
        ForEach-Object { $_.Name.ToLowerInvariant() })
    foreach ($dependency in (Get-PeDependencies $dumpbin $pe.FullName)) {
        if ($dependency.StartsWith('api-ms-win-') -or $dependency.StartsWith('ext-ms-win-')) { continue }
        if ($dependency -in $systemDlls) { continue }
        if ($dependency -notin $localDllNames) {
            throw "Unresolved non-system dependency '$dependency' imported by '$($pe.FullName)'."
        }
    }
}

$baseline = (Get-Content -LiteralPath (Join-Path $repository 'vcpkg-configuration.json') -Raw | ConvertFrom-Json).'default-registry'.baseline
$status = Read-VcpkgStatus $vcpkgStatusPath
# OCCT is absent from the root status because only the dedicated STEP host
# manifest installs it; take its version/ABI from that tree's SPDX record.
$stepSpdx = Get-Content -LiteralPath (Join-Path $stepVcpkgRoot 'share\opencascade\vcpkg.spdx.json') -Raw | ConvertFrom-Json
$occtPackage = $stepSpdx.packages | Where-Object { $_.name -eq 'opencascade' } | Select-Object -First 1
$occtAbiPackage = $stepSpdx.packages | Where-Object { $_.name -eq 'opencascade:x64-windows-static-md' } | Select-Object -First 1
if ($null -eq $occtPackage) { throw 'The STEP-host OCCT SPDX record has no opencascade package.' }
$components = @()
foreach ($name in $thirdParty) {
    $entry = $null
    $abi = $null
    if ($name -eq 'opencascade') {
        $packageVersion = $occtPackage.versionInfo
        if ($null -ne $occtAbiPackage) { $abi = $occtAbiPackage.versionInfo }
    } else {
        if (-not $status.ContainsKey($name)) { throw "vcpkg status has no entry for '$name'." }
        $entry = $status[$name]
        $packageVersion = if ($entry.ContainsKey('Version')) { $entry.Version } elseif ($entry.ContainsKey('Version-Semver')) { $entry.'Version-Semver' } else { 'unknown' }
        if ($entry.ContainsKey('Port-Version') -and $entry.'Port-Version' -ne '0') { $packageVersion = "$packageVersion#$($entry.'Port-Version')" }
        if ($entry.ContainsKey('Abi')) { $abi = $entry.Abi }
    }
    $component = [ordered]@{
        type = 'library'
        name = $name
        version = $packageVersion
        'bom-ref' = "pkg:vcpkg/$name@${packageVersion}?triplet=x64-windows-static-md"
        licenses = @(@{ license = @{ name = "See licenses/$name.txt" } })
        properties = @(
            @{ name = 'preview3d:vcpkg-baseline'; value = $baseline },
            @{ name = 'preview3d:architecture'; value = 'x64-windows-static-md' }
        )
    }
    if ($abi) {
        $component.properties += @{ name = 'preview3d:vcpkg-abi'; value = $abi }
    }
    $components += $component
}
$crtVersion = (Get-Item -LiteralPath (Join-Path $crtDirectory 'vcruntime140.dll')).VersionInfo.ProductVersion
$crtToolsetDirectory = Split-Path (Split-Path $crtDirectory -Parent) -Parent
$crtToolsetVersion = Split-Path $crtToolsetDirectory -Leaf
$components += [ordered]@{
    type = 'library'
    name = 'Microsoft Visual C++ Runtime'
    version = $crtVersion
    'bom-ref' = "microsoft-vc-runtime-$crtVersion-x64"
    licenses = @(@{ license = @{ name = 'Microsoft Visual Studio distributable code terms' } })
    properties = @(@{ name = 'preview3d:provenance'; value = "Visual Studio MSVC $crtToolsetVersion x64 app-local CRT" })
}
$sbom = [ordered]@{
    bomFormat = 'CycloneDX'
    specVersion = '1.5'
    serialNumber = "urn:uuid:$([guid]::NewGuid())"
    version = 1
    metadata = [ordered]@{
        timestamp = (Get-Date).ToUniversalTime().ToString('o')
        component = @{ type = 'application'; name = 'Preview3D'; version = $Version }
        properties = @(
            @{ name = 'preview3d:configuration'; value = 'Release' },
            @{ name = 'preview3d:platform'; value = 'x64' },
            @{ name = 'preview3d:distribution'; value = $distributionDirectory },
            @{ name = 'preview3d:vcpkg-baseline'; value = $baseline },
            @{ name = 'preview3d:signed'; value = $signed.ToString().ToLowerInvariant() }
        )
    }
    components = $components
}
$sbom | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $stage 'SBOM.cdx.json') -Encoding UTF8

$forbiddenNames = @(
    'Preview3DThumbnailProvider.dll',
    'Preview3DHostileWorker.exe', 'Tests.Unit.exe', 'Tests.ImportIsolation.exe'
)
$stagedNames = @(Get-ChildItem -LiteralPath $stage -File -Recurse | ForEach-Object { $_.Name })
foreach ($forbidden in $forbiddenNames) {
    if ($forbidden -in $stagedNames) { throw "Excluded payload '$forbidden' entered the stage." }
}
$allowedOpenUsdHostPaths = @($openUsdHostFiles + $openUsdHostCrt | ForEach-Object { $_.Replace('\', '/').ToLowerInvariant() })
$stagedOpenUsdHostPaths = @(Get-ChildItem -LiteralPath $openUsdHostStage -File -Recurse | ForEach-Object {
    $_.FullName.Substring($openUsdHostStage.Length + 1).Replace('\', '/').ToLowerInvariant()
})
if (@($allowedOpenUsdHostPaths | Where-Object { $_ -notin $stagedOpenUsdHostPaths }).Count -ne 0 -or
    @($stagedOpenUsdHostPaths | Where-Object { $_ -notin $allowedOpenUsdHostPaths }).Count -ne 0) {
    throw 'The private OpenUsdHost payload does not match its closed release allowlist.'
}
$allowedStepHostPaths = @($stepHostFiles + $stepHostCrt | ForEach-Object { $_.Replace('\', '/').ToLowerInvariant() })
$stagedStepHostPaths = @(Get-ChildItem -LiteralPath $stepHostStage -File -Recurse | ForEach-Object {
    $_.FullName.Substring($stepHostStage.Length + 1).Replace('\', '/').ToLowerInvariant()
})
if (@($allowedStepHostPaths | Where-Object { $_ -notin $stagedStepHostPaths }).Count -ne 0 -or
    @($stagedStepHostPaths | Where-Object { $_ -notin $allowedStepHostPaths }).Count -ne 0) {
    throw 'The private StepHost payload does not match its closed release allowlist.'
}
$debugRuntimePattern = '^(?:msvcp140d(?:_atomic_wait|_codecvt_ids)?|msvcp140_[12]d|vcruntime140d|vcruntime140_1d|concrt140d|ucrtbased)\.dll$'
if (Get-ChildItem -LiteralPath $stage -File -Recurse | Where-Object { $_.Name -match $debugRuntimePattern -or $_.Extension -in @('.pdb', '.lib') }) {
    throw 'A debug runtime, PDB, or import/static library entered the release stage.'
}

$manifestEntries = @(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{
        path = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        bytes = $_.Length
        sha256 = Get-Sha256 $_.FullName
    }
})
$manifest = [ordered]@{
    schema = 1
    product = 'Preview3D'
    version = $Version
    configuration = 'Release'
    platform = 'x64'
    distribution = $distributionDirectory
    signed = $signed
    vcpkg_baseline = $baseline
    generated_utc = (Get-Date).ToUniversalTime().ToString('o')
    files = $manifestEntries
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $stage 'MANIFEST.json') -Encoding UTF8

# MANIFEST.json intentionally cannot hash itself. Every other payload is
# covered by that manifest. Portable output additionally covers the complete
# archive (including the manifest) with the adjacent SHA-256 file; installer
# output is finalized and checksummed by Create-Installer.ps1.
if ($Distribution -eq 'Portable') {
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -CompressionLevel Optimal
    $archiveHash = Get-Sha256 $archive
    "$archiveHash  $([System.IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath $archiveChecksum -Encoding ASCII
    Write-Host "Portable release: $archive"
    Write-Host "SHA-256: $archiveHash"
} else {
    Write-Host "Installer payload: $stage"
}
Write-Host "Staged files: $((Get-ChildItem -LiteralPath $stage -File -Recurse).Count)"

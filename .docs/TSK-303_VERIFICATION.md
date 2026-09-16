# TSK-303 portable-release verification

Date: 2026-09-16

## Result

The packaging implementation is complete and the required solution command
produces exactly one clean portable ZIP plus its adjacent checksum. The current
artifact is an **unsigned engineering archive**, not a release candidate:

```text
artifacts\portable\Preview3D-0.1.0-portable-x64.zip
SHA-256 156eed9a52b585f84f51c485e5ab0e69961c5a73130fc90709456deceaa07118
```

Local extracted-package tests pass for GLB, glTF with a local binary sidecar,
binary STL, little-endian PLY mesh, and big-endian PLY points. AppContainer
provisioning and cleanup also pass without elevation. A signing certificate and
a clean offline Windows 11 VM were not available here. Those two acceptance
rows remain explicitly open for TSK-305; this document does not turn the local
engineering result into a signed/clean-machine claim. TSK-302's large-model
failures are independent blockers and are not waived by successful packaging.

## Entry point and build ordering

`Directory.Solution.targets` adds `CreatePortableRelease` to MSBuild's generated
solution metaproject. It invokes `packaging\CreatePortableRelease.proj` once.
That project rejects configurations other than `Release|x64`, builds only
`interactive-viewer\Preview3D.vcxproj`, and waits for its worker project
reference (including vcpkg app-local deployment) before invoking the packager
once. No packaging target is imported into ordinary project builds.

The required command passed:

```powershell
msbuild Preview3D.slnx /t:CreatePortableRelease `
  /p:Configuration=Release /p:Platform=x64
```

The log contained only `Preview3DImportWorker.vcxproj` and `Preview3D.vcxproj`
product builds before the singular archive message. The target does not build
or stage the test, thumbnail, or compatibility-host projects.

A subsequent ordinary Release solution build passed and did not change the
archive timestamp or hash, proving packaging is not an every-build side effect.
The freshly rebuilt Release Catch2 binaries then passed 87 cases / 7,239
assertions (Unit) and 200 cases / 51,943 assertions (ImportIsolation). PowerShell
AST parsing and MSBuild XML parsing also passed for the new scripts/projects.

## Clean allowlist and dependency closure

The packager deletes only its validated
`artifacts\portable\stage` child, reconstructs it from an explicit allowlist,
and refuses missing inputs, debug runtimes, PDB/LIB files, or excluded binary
names. Stale files in the shared `x64\Release` output do not enter the package.
The current ZIP contains 31 files:

| Location | Payload |
| --- | --- |
| root | `Preview3D.exe`; the four directly imported app-local MSVC runtime DLLs; README, cleanup tool, notices, SBOM, and manifest |
| `worker\` | `Preview3DImportWorker.exe`; Draco, fastgltf, KTX, libsharpyuv, libwebp, meshoptimizer, simdjson, zstd; the four worker-imported MSVC runtime DLLs |
| `licenses\` | verbatim installed-port copyright/license records for Basis Universal, Draco, fastgltf, KTX, libwebp, meshoptimizer, simdjson, and zstd |

The archive has no tests, hostile worker, compatibility host, thumbnail DLL,
PDB/import library, debug CRT, source, fixture, cache, or MSI payload. `dumpbin
/dependents` is run over every staged PE. Each non-API-set import must be either
an exact Windows 11 system-DLL allowlist member or a DLL beside the importing
binary. That last locality check matters because the loader does not use the
viewer's parent directory to resolve the worker's dependencies.

The retained Windows graphics/image dependencies are inbox `d3d11`, `d3d12`,
DXGI, D3DCompiler 47, Direct2D, DirectWrite, and WIC. No Agility SDK is used.
The non-inbox MSVC runtime is staged app-local. The current CycloneDX 1.5 SBOM
records the pinned vcpkg baseline
`04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4`, installed package ABI values, and:

| Component | Installed version |
| --- | --- |
| Basis Universal | 2.50 |
| Draco | 1.5.7#1 |
| fastgltf | 0.9.0 |
| KTX-Software | 4.4.2 |
| libwebp/libsharpyuv | 1.6.0#3 |
| meshoptimizer | 1.2 |
| simdjson | 4.6.8 |
| zstd | 1.5.7 |
| Microsoft Visual C++ Runtime x64 | 14.51.36247.0 |

`MANIFEST.json` hashes every other staged file. The adjacent `.sha256` matches
the complete ZIP. The observed manifest/stage counts were 30/31 (the manifest
cannot recursively hash itself), SBOM component count was 9, and ZIP checksum
verification returned true.

## Extracted-package execution

The final archive above was expanded into a new directory and run from there,
not from `x64\Release`. Tests ran in a medium-integrity process; membership in
`BUILTIN\Administrators` was deny-only, so no elevated token was used.

Each command used the extracted `Preview3D.exe`, a result file in the extracted
directory, `--benchmark-duration-ms=3000`, `--benchmark-frames=5000`, one repeat,
and the compatibility reference. All returned exit 0/status `pass`:

| Fixture | First geometry | Worker peak private commit |
| --- | ---: | ---: |
| A-small GLB | 437.716 ms | 3,690,496 bytes |
| external-buffer `.gltf` sidecar | 373.232 ms | 3,473,408 bytes |
| A-small binary STL | 379.395 ms | 3,424,256 bytes |
| A-small little-endian PLY mesh | 378.388 ms | 3,514,368 bytes |
| A-small big-endian PLY points | 374.572 ms | 3,436,544 bytes |

The values are smoke evidence, not a replacement for TSK-302's repeated
reference-system qualification. No viewer or worker process remained after the
runs.

The packaged viewer resolved `worker\Preview3DImportWorker.exe`; the existing
same-directory lookup remains only as the build/test fallback. After first
import, the extracted root had zero explicit AppContainer SID ACEs and
`worker\` had the two expected read/execute ACE forms (directory and inheritable
object/container access). `Remove-Preview3DProfile.ps1` returned 0, removed both
worker-directory ACEs, deleted the current-user profile, and was idempotent on a
second run. Closing each smoke also left zero worker processes.

This proves the first-use/current-user provisioning path and its narrow target
on this machine. It does not prove the stronger clean-machine statement that no
pre-existing developer ACL/profile can exist; that is why the clean VM row
below remains open.

## Signing and final clean-VM procedure

An engineering package with no certificate emits a warning and records
`"signed": false` in both manifest/SBOM metadata. The final candidate command is:

```powershell
msbuild Preview3D.slnx /t:CreatePortableRelease `
  /p:Configuration=Release /p:Platform=x64 `
  /p:PortableSigningThumbprint=<SHA1> `
  /p:PortableTimestampUrl=https://timestamp.digicert.com
```

The packager signs the staged viewer and worker with SHA-256 plus RFC 3161
timestamping, verifies both with `signtool /pa /all`, then produces the SBOM,
file hashes, ZIP, and ZIP checksum. Signing happens on staged copies so it does
not mutate shared build output. TSK-305 must retain the signing log and verify
the signatures again after extraction.

Still required on a clean Windows 11 x64 VM, logged in as a standard user with
no Visual Studio/vcpkg and with networking disabled:

1. Verify the ZIP checksum and both Authenticode signatures, then extract to a
   Unicode path owned by that user.
2. Run GLB, external-sidecar glTF, binary STL, both-endian PLY mesh/points,
   compressed/texture samples, malformed/over-limit errors, and reopen after
   failure. Confirm no dependency load or network attempt.
3. Confirm the AppContainer profile is current-user only, root/source/model
   directories have no new SID ACE, only `worker\` has read/execute, and direct
   path/network/process access stays denied while brokered handles work.
4. Close during load and after Ready; verify viewer/worker cleanup. Run the
   profile cleanup tool, verify its ACE/profile removal, then delete the
   extracted directory without elevation or residue other than recorded test
   logs outside that directory.
5. Confirm the archive still contains only the allowlist and reconcile every
   binary/hash/license/SBOM component against the signed candidate manifest.

Until that matrix and the retained performance gates pass, portable delivery is
implemented but the scope-limited MVP is not release-ready.

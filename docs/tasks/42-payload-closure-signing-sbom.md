# T42 — Package, sign, and audit the provider payload closure

## Goal
Ship the provider with an auditable, signed payload: the DLL plus every family runtime it needs,
covered by a closed PE-dependency allowlist, notices, SBOM, signing and a per-file hash manifest.

## Context (read first)
- `docs/design/08-installation-and-registration.md` — dependency/registration ownership.
- `docs/design/02-system-architecture.md` — which libraries may appear in the thumbnail closure.
- `docs/design/adr/0002` (OCCT), `0003` (decoders), `0004` (source sharing) — what the DLL links.
- `packaging/installer/Preview3D.nsi`, `packaging/installer/Create-Installer.ps1`, `packaging/portable/Create-PortableRelease.ps1`, `packaging/portable/THIRD-PARTY-NOTICES.txt` — the existing packaging scripts to extend.

## Scope
- [x] Stage the provider DLL and every non-system runtime it resolves (e.g. ufbx/lib3mf/zlib/TinyUSDZ/Draco/KTX2/libwebp/meshopt and, for STEP, the constrained OCCT modules) in the installer payload.
- [x] Extend the PE-dependency closure allowlist to include exactly those modules (plus the known Windows system DLLs) and fail staging on any unresolved or unexpected non-system import.
- [x] Add the provider's dependency notices and SBOM entries.
- [x] Wire Authenticode signing for the DLL and a per-file SHA-256 manifest into the release procedure.
- [x] Prove the DLL imports no viewer/worker/host binary and that the viewer/worker/hosts import no provider-only library.

## Out of scope
- Registration keys (→ T41).
- The portable ZIP scope decision (ADR-016): the provider is an installed feature and remains excluded from the portable archive. A later scope change requires an ADR.
- The full MSI/SBOM gate 7 (viewer program).

## Design notes
- vcpkg autolink places the whole triplet's `lib\*.lib` on the link line; verify the real closure with `dumpbin`, not the manifest.
- Keep the payload allowlist closed (explicit list), never "everything in `$(OutDir)`".
- No signing certificate may be available locally; record the unsigned-engineering state honestly if so.

## Done when
- [x] The installer stages the provider payload and the closure check passes with no unresolved non-system import.
- [x] Notices, SBOM entries and the hash manifest include every provider binary.
- [x] Commands and results are recorded in Hand-off.
- [x] Hand-off below filled in.

## Hand-off

### What landed
- `packaging/portable/Create-PortableRelease.ps1`:
  - stages `Preview3DThumbnailProvider.dll` at the install root for `-Distribution Installer`
    only; the portable archive still forbids it (ADR-0016);
  - adds `msvcp140_1.dll` to the root app-local CRT closure;
  - adds a **closed PE import allowlist** for the provider: `dumpbin /dependents` on the staged DLL
    must be a subset of the Windows system DLLs already known to the script plus
    `{msvcp140.dll, msvcp140_1.dll, vcruntime140.dll, vcruntime140_1.dll}`; any other non-system
    import fails staging, and any missing CRT import fails staging;
  - adds **boundary checks**: no provider-only static-closure module name (`fastgltf/draco/ktx/
    libwebp/libsharpyuv/basisu/meshoptimizer/ufbx/lib3mf/libzip/zip/z/bz2/tinyusdz/simdjson/zstd`)
    and no `TK*.dll` may appear anywhere in the payload; no product binary may import the provider
    and the provider may not import a product binary;
  - adds a `Preview3DThumbnailProvider` component to `SBOM.cdx.json` for the Installer distribution
    with `preview3d:statically-links` and `preview3d:runtime-imports` properties.
- `packaging/installer/Preview3D.nsi`: `File` lines for the provider DLL and `msvcp140_1.dll`, with
  matching `Delete` lines in the uninstall section. The provider still registers from the stable
  `[INSTALLFOLDER]Preview3DThumbnailProvider.dll` path (T41).
- `packaging/installer/Create-Installer.ps1`: builds `thumbnail-provider\Preview3DThumbnailProvider.vcxproj`
  in Release x64 as part of the release procedure (the provider is deliberately not a viewer
  `ProjectReference`).
- `packaging/portable/THIRD-PARTY-NOTICES.txt`: names the provider and states its upstream closure is
  statically linked.
- `tests/app-smoke/provider_package.py` (new): static packaging contract (NSIS File/Delete,
  Installer-only staging, allowlist/boundary presence, notices/SBOM) plus stage verification of every
  `MANIFEST.json` hash, provider + CRT presence at the root, no leaked provider-only modules, and the
  SBOM component.

### The real closure (dumpbin, not the manifest)
The release triplet statically links the provider's bounded parser/decoder copies and the constrained
OCCT closure. `dumpbin /dependents` on the built DLL shows only `GDI32, ole32, KERNEL32, MSVCP140,
MSVCP140_1, bcrypt, VCRUNTIME140(_1), WS2_32, ADVAPI32, USER32` and `api-ms-win-crt-*` UCRT modules.
There are **no** standalone `ufbx`/`lib3mf`/`zlib`/`zip`/`bz2`/`draco`/`ktx`/`basisu`/`libwebp`/
`meshoptimizer`/`tinyusdz`/OCCT DLLs to stage — so the provider payload addition is the DLL plus
`msvcp140_1.dll` (the other CRT files were already at the root for the viewer). This is exactly the
case the task design note anticipated ("verify the real closure with `dumpbin`, not the manifest").

### Deviations and why
- The scope line lists ufbx/lib3mf/zlib/TinyUSDZ/Draco/KTX2/libwebp/meshopt and the constrained OCCT
  modules as runtimes to stage. They are statically linked (commit `4f89dff`, `Directory.Build.props`
  `VcpkgUseStatic`), so staging them as DLLs would regress the Smart App Control / SignPath rationale
  and the static-closure release design. The closed allowlist therefore accepts only the CRT as the
  provider's non-system import and fails on anything else.
- No Authenticode certificate is available locally, so this run is an **unsigned engineering
  candidate**: `Create-PortableRelease.ps1` sets `preview3d:signed=false` in the SBOM, `MANIFEST.json`
  hashes every staged binary, and `Create-Installer.ps1` emits the setup `.sha256` and warns. Signing
  is fully wired (per-thumbprint, signs every payload PE, skips already-trusted CRT images) and
  verified once a thumbprint is supplied.

### Environment note (pre-existing, not a T42 source change)
The committed packaging already expects the dedicated STEP host to be statically linked from
`compatibility-host-step\vcpkg_installed\x64-windows-static-md`. The local tree held only an older
dynamic `x64-windows` install and the `Preview3DStepHost.exe` was a stale dynamic build, so a full
stage build failed before any provider work. The local static OCCT tree was re-provisioned (a fresh
`vcpkg install` hit a vcpkg tool/port version skew, so the already-built provider static OCCT tree was
reused via a directory junction) and `Preview3DStepHost.exe` was rebuilt static (now imports only the
CRT + system DLLs). CI provisions the tree from `.github/workflows/dependencies.yml`; no T42 source
change depends on this.

### Check results
- `python tests/app-smoke/provider_package.py artifacts/installer/stage` → pass, "66 manifest entries
  verified".
- `pwsh packaging/portable/Create-PortableRelease.ps1 -RepositoryRoot . -Distribution Installer` →
  pass, "Installer payload: ...\artifacts\installer\stage", "Staged files: 67".
- `pwsh packaging/installer/Create-Installer.ps1 -RepositoryRoot . -SkipBuild` → pass; setup built:
  `artifacts\installer\Preview3D-0.3.10-x64-setup.exe`,
  SHA-256 `9c355c0aac11c0b3a30f2023b2db40a2881737593211bbf1d296b1c4fb02fee5` (unsigned warning).
- `pwsh tests/unit/check-provider-dependency-closure.ps1 -Configuration Release` → "OK: no
  viewer/worker/host/core imports (22 imported modules checked)".
- `pwsh packaging/smoke/Stage-ProviderSmoke.ps1 -RepositoryRoot .` → staged 8 files, closure OK.
- `python tests/app-smoke/thumbnail_registration.py` → pass (T41 contract intact).
- `x64\Release\Tests.Unit.exe` → "All tests passed (134916 assertions in 339 test cases)".

### Remaining work / next task must know
1. T44 must verify the signed NSIS install on a clean machine and that `InprocServer32` resolves to
   `[INSTALLFOLDER]\Preview3DThumbnailProvider.dll` (the exact path staged here) loading in
   `DllHost.exe` with no `DisableProcessIsolation`.
2. The provider is a single self-contained image and ships only in the installed distribution. Adding
   any provider-only runtime as a standalone payload requires deliberately extending the closed
   allowlist in `Create-PortableRelease.ps1` (and the SBOM/notices), not shipping it implicitly.
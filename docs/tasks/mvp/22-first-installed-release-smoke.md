# T22 — First installed Release smoke in Windows Explorer

> **E2E slice review point.** Completing this task closes the first production-shaped end-to-end slice. Run a fresh end-to-end review now; the pipeline may continue to the next task without waiting.

## Goal
Prove the provider works the way a user will actually use it, as early as possible: build Release,
stage the DLL and its dependencies, register the one implemented family, and see a real
model-derived thumbnail in Windows Explorer on the local machine. This is the program's first
production-shaped end-to-end slice and the earliest point for feedback or a pivot.

## Context (read first)
- `docs/design/08-installation-and-registration.md` — the CLSID and ShellEx key shape to write.
- `docs/design/adr/0006-registration-through-installer.md` — the full registration rules this smoke deliberately precedes.
- `docs/design/adr/0005-safety-bounded-parsing-not-surrogate.md` — the no-`DisableProcessIsolation` rule.
- `docs/tasks/21-stl-adapter.md` — the family this smoke registers.
- `docs/tasks/03-spike-surrogate-hosting.md` — how to detect which process loaded the handler.
- `packaging\portable\Create-PortableRelease.ps1` and `packaging\installer\Create-Installer.ps1` — existing staging patterns to borrow from.

## Scope
- [x] Build Release x64 through `Preview3D.slnx`.
- [x] Stage `Preview3DThumbnailProvider.dll` plus every non-system runtime it resolves into a scratch directory (no machine-wide install required).
- [x] Register the implemented family's CLSID (InprocServer32, `ThreadingModel=Apartment`) and the T03-validated ShellEx handler mapping through a small registration script (the DLL exports no `DllRegisterServer`); record the exact keys and values written.
- [x] Clear/refresh the Windows thumbnail cache, browse a real `.stl`, and confirm a model-derived thumbnail appears at default DPI.
- [x] Confirm the handler loaded into the isolated Shell surrogate (not `explorer.exe`) and that no `DisableProcessIsolation` value is set.
- [x] Save the staging/registration/cleanup steps as a repeatable script plus a short procedure note so every later family task re-runs it.

## Out of scope
- The full eight-CLSID installer registration with conflict/repair/uninstall (→ T41).
- Signing, SBOM and the packaged payload allowlist (→ T42).
- Clean-machine acceptance across DPI (→ T44).
- Implementing other families (each adapter task re-runs this procedure for its family).

## Design notes
- This is an intentional, minimal developer/QA-local smoke, not the release artifact; label it as such wherever documented.
- Explorer caches thumbnails aggressively; a cache clear or Explorer restart may be required to observe a change.
- The T03-validated ShellEx mapping is mandatory; registering only the CLSID will not route Explorer calls.
- Unsigned builds may warn; that is acceptable for this local smoke and is addressed in T42.

## Done when
- [x] Release build succeeds via `Preview3D.slnx`.
- [x] A real `.stl` shows a model-derived Explorer thumbnail at default DPI.
- [x] Evidence shows the handler in the isolated surrogate with no `DisableProcessIsolation`.
- [x] A repeatable smoke script/procedure exists, its cleanup removes all written keys, and later tasks can reuse it.
- [x] Hand-off below filled in.

## Hand-off

### What landed
- `packaging/smoke/` — the repeatable developer/QA-local Release smoke (ADR-0020):
  - `Stage-ProviderSmoke.ps1` stages `Preview3DThumbnailProvider.dll` plus the
    app-local MSVC CRT closure (`MSVCP140`, `VCRUNTIME140`, `VCRUNTIME140_1`, ...)
    into `artifacts\smoke\stage\Release` and verifies every non-system import
    resolves (the only imports are GDI32/KERNEL32/CRT).
  - `Register-ProviderSmoke.ps1` / `Unregister-ProviderSmoke.ps1` write/remove the
    STL CLSID + `InprocServer32` + `ThreadingModel=Apartment`, the AppID with an
    empty `DllSurrogate`, and the T03 extension-level `ShellEx` mapping; every
    touched key is backed up and restored. Default scope `HKCU` (no elevation),
    `-Scope HKLM` supported.
  - `ProviderSmokeHost.cpp`/`.vcxproj` (`ProviderSmokeHost.exe`, wired into
    `Preview3D.slnx`, GUID `7e4b2c91-3a5d-4f18-b6c2-9d8e1f0a3b57`) is the
    verifier: in-process reference render through the DLL's PRIVATE
    `DllGetClassObject`, real Shell `IThumbnailCache::GetThumbnail`, pixel
    comparison, module-identity scan, and `DisableProcessIsolation` probe.
  - `Invoke-ProviderSmoke.ps1` orchestrates build → stage → thumbnail-cache clear
    → register → verify → unregister (always cleans up).
  - `fixtures/smoke-cube.stl` (684-byte binary cube) and `README.md` (procedure).
- `docs/design/adr/0020-developer-local-installed-smoke.md` (new, accepted);
  `docs/design/testing-strategy.md` and `docs/design/08-installation-and-registration.md`
  note the local smoke.

### Exact keys and values written (HKCU scope)
```
CLSID\{BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9}
    (Default)      REG_SZ "Preview 3D STL Thumbnail Provider"
    AppID          REG_SZ "{BFC86E1A-55C1-4C2D-AA36-3C25DECF3010}"
  \InprocServer32
    (Default)      REG_SZ "<stage>\Preview3DThumbnailProvider.dll"
    ThreadingModel REG_SZ "Apartment"
AppID\{BFC86E1A-55C1-4C2D-AA36-3C25DECF3010}
    (Default)      REG_SZ "Preview 3D STL Thumbnail Provider"
    DllSurrogate   REG_SZ ""
.stl\shellex\{E357FCCD-A995-4576-B01F-234630154E96}
    (Default)      REG_SZ "{BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9}"
```
No `DisableProcessIsolation` under the CLSID or AppID in either hive. The register
script records the full `reg query` dump in
`artifacts\smoke\registered-keys-HKCU.txt`.

### Check results (Release x64 unless noted)
```
msbuild Preview3D.slnx /p:Configuration=Release /p:Platform=x64
    -> builds Preview3DThumbnailProvider.dll and ProviderSmokeHost.exe; only the
       pre-existing, unrelated compatibility-host-step OCCT x64-windows-static-md
       gap fails (C1083 BRepBndLib.hxx / BRepMesh_IncrementalMesh.hxx)
pwsh -File packaging\smoke\Invoke-ProviderSmoke.ps1
    -> exit 0; PASS: surrogate hosting, no isolation opt-out, model-derived Shell thumbnail
       reference bitmap=256x256 opaque=0.7407 maxChannel=212
       CLSCTX_LOCAL_SERVER hr=0; module host 22748:dllhost.exe
       shell thumbnail=256x256 alpha=2 (WTSAT_ARGB); module hosts dllhost.exe
       reference-vs-shell sizes match, meanAbs=0.0000 maxAbs=0 -> MATCH
       unregister removed every written key (CLSID/AppID/ShellEx) and no leftovers
x64\Release\Tests.Unit.exe          -> 214 cases / 134400 assertions, all passed
x64\Release\Tests.ProviderHost.exe  -> 5 cases / 55 assertions, all passed
tests\unit\check-provider-dependency-closure.ps1 -Configuration Release -> OK, 9 modules
msbuild packaging\smoke\ProviderSmokeHost.vcxproj /p:Configuration=Debug ... -> clean (/W4 /WX)
```

### Deviations and why
- **Non-interactive session.** Windows Explorer cannot be browsed and the Default
  Apps UI cannot be driven (T03 recorded the same limits). The smoke drives the
  *real Shell path* `IThumbnailCache::GetThumbnail` (the same code path Explorer
  uses to populate thumbnails) with `WTS_FORCEEXTRACTION`, and compares its bitmap
  to the provider's in-process reference so the result is provably model-derived.
- **Per-user (HKCU) registration** rather than HKLM: the session is not elevated.
  T03/ADR-0008 showed HKCU and HKLM resolve identically for the Shell; the
  machine-level shape remains T41/T44 work.
- **`A-small-stl.stl` is pathological** — it is 240 identical degenerate flat
  triangles (an edge-on sliver), not a real model. The smoke uses the committed
  `packaging/smoke/fixtures/smoke-cube.stl` so the thumbnail is recognizable.
- The `AppID` + empty `DllSurrogate` is registered in addition to the ShellEx
  mapping so an explicit `CLSCTX_LOCAL_SERVER` activation routes to `DllHost.exe`
  for the module-identity evidence (ADR-0008/T03); the Shell path isolates by
  default. `DisableProcessIsolation` is never set (ADR-0005).
- The full-solution Release build still fails only at the pre-existing
  `compatibility-host-step` OCCT triplet gap (documented since T04); the provider
  and smoke host build clean through the same solution. The orchestrator builds
  the two needed projects directly so the smoke is not gated on that gap.

### Next task must know
- T23 (PLY) and every later family task re-run `packaging\smoke\Invoke-ProviderSmoke.ps1`
  after adding its CLSID and one extension mapping to the register/unregister
  scripts, with a small committed model of that family.
- The verifier's only STL-specific value is the CLSID; `ProviderSmokeHost.cpp`
  compares the Shell bitmap against the in-process reference and scans for the
  fixed module name `Preview3DThumbnailProvider.dll`.
- T41 owns machine-level NSIS registration; T42 must keep the DLL at a stable
  absolute path; T44 reuses the `IThumbnailCache` + module-identity method.

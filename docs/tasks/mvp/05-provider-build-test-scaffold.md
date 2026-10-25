---
verify: x64\Release\Tests.Unit.exe
---
# T05 — Scaffold the provider build and test integration

## Goal
Turn `Preview3DThumbnailProvider.vcxproj` from a stub into a correctly configured DLL project that
builds in both configurations through the solution, deploys its app-local dependencies, and is
covered by a runnable test target.

## Context (read first)
- `thumbnail-provider/Preview3DThumbnailProvider.vcxproj` — the current stub project (no hardening flags, no exports).
- `Preview3D.slnx` — the only supported build entry point.
- `docs/design/05-thumbnail-provider.md` — required compile flags (`/guard:cf`, `/CETCOMPAT`, `/DYNAMICBASE`, `/NXCOMPAT`, `/sdl`, high warning level) and export surface.
- `docs/design/testing-strategy.md` — build-through-solution rule and test layers.
- `tests/unit/Tests.Unit.vcxproj` — the existing Catch2 target to extend or mirror.

## Scope
- [ ] Apply the hardening/security compile and link flags and warnings-as-errors at the product boundary.
- [ ] Add a `.def` (or `__declspec(dllexport)`) exporting exactly `DllGetClassObject` and `DllCanUnloadNow`. Do **not** export `DllRegisterServer`/`DllUnregisterServer`; registration is installer-owned (ADR-0006/0007, `05-thumbnail-provider.md`).
- [ ] Add the provider unit/COM test group inside `Tests.Unit.vcxproj` (provider tests live in `Tests.Unit.exe`, per `testing-strategy.md`), linking the provider DLL, and record the frozen name `Tests.ProviderHost.exe` for the COM host harness target that T17 creates and wires into `Preview3D.slnx`.
- [ ] Add an automated dependency-closure check (`dumpbin /dependents`) asserting the DLL imports no viewer/worker/host binary.
- [ ] Confirm Debug and Release x64 solution builds succeed and `Tests.Unit.exe` runs from the repository root.
- [ ] Confirm `thumbnail-provider\Preview3DThumbnailProvider.vcxproj.user` and `thumbnail-provider\x64\` are ignored/untracked (they are not in source control today); ensure the root build policy keeps only x64 Debug/Release configurations.

## Out of scope
- Implementing COM behavior (→ T11).
- Packaging/signing the payload (→ T42).
- Registration (→ T41).

## Design notes
- Build only through `Preview3D.slnx`; a project-level build leaves `$(SolutionDir)` undefined.
- The DLL must not link the viewer, `Preview3DImportWorker.exe`, either import host, or the persistent cache.
- Keep test output project-private so an incremental clean cannot delete sibling Release artifacts.

## Done when
- [ ] `msbuild Preview3D.slnx /p:Configuration=Debug /p:Platform=x64` and the Release equivalent both succeed with zero warnings.
- [ ] `x64\Release\Tests.Unit.exe` runs from the repository root with zero failures (the `verify:` command).
- [ ] `dumpbin /dependents` shows no viewer/worker/host import; the two-export surface is confirmed.
- [ ] Commands and results are recorded in Hand-off.
- [ ] Hand-off below filled in.

## Hand-off

### What landed
- `thumbnail-provider/Preview3DThumbnailProvider.def` — exports exactly `DllGetClassObject` and
  `DllCanUnloadNow`, both `PRIVATE`; no `DllRegisterServer`/`DllUnregisterServer`.
- `thumbnail-provider/ProviderExports.cpp` — T05 stub bodies (`DllGetClassObject` →
  `CLASS_E_CLASSNOTAVAILABLE`, `E_POINTER` on a null out-param; `DllCanUnloadNow` → `S_OK`). T11
  replaces the bodies; the signatures and the two-symbol surface are frozen (ADR-0010).
- Hardening in `Preview3DThumbnailProvider.vcxproj` (both configs): compile `/guard:cf`
  (`ControlFlowGuard=Guard`), link `/CETCOMPAT`, `/DYNAMICBASE`, `/NXCOMPAT`
  (`CETCompat`/`RandomizedBaseAddress`/`DataExecutionPrevention`), and `ModuleDefinitionFile`. `/sdl`,
  `/W4` and warnings-as-errors already come from `Directory.Build.props`. Debug now uses `/Zi`
  (`DebugInformationFormat=ProgramDatabase`) because `/guard:cf` rejects the `/ZI` Edit-and-Continue
  default.
- `tests/unit/ProviderScaffoldTests.cpp` + `PREVIEW3D_PROVIDER_DLL` macro + a build-order-only
  `ProjectReference` to the provider in `Tests.Unit.vcxproj`. The tests load the built DLL and assert:
  exactly `{DllCanUnloadNow, DllGetClassObject}` exported, no `DllRegisterServer`/`DllUnregisterServer`,
  no import whose name starts with `Preview3D` (viewer/worker/hosts/OpenUSD core), and that both entry
  points are callable.
- `tests/unit/check-provider-dependency-closure.ps1` — `dumpbin /dependents` wrapper, the literal
  evidence command for the closure check.
- `docs/design/adr/0010-provider-build-export-and-test-boundary.md`; `docs/design/testing-strategy.md`
  updated to reference the automated closure/export checks and the frozen host name.

### Deviations
- "Linking the provider DLL" is implemented as a build-order-only `ProjectReference` plus runtime
  `LoadLibrary`/`GetProcAddress`, not an import-library link: the two COM entry points are `PRIVATE`
  (no import library), and this is exactly how the Shell and the T17 host activate the in-proc server.
- `Tests.ProviderHost.exe` is only **recorded** here (name frozen, ADR-0010); T17 creates and wires it
  into `Preview3D.slnx`, as scoped.
- The full-solution build remains red **only** on `compatibility-host-step` (`C1083 BRepBndLib.hxx`/
  `BRepMesh_IncrementalMesh.hxx`): its hardcoded `vcpkg_installed\x64-windows-static-md` include path
  does not exist (only `x64-windows` is installed there). This is the same pre-existing, unrelated gap
  T04 recorded and reported done against; it is independent of T05. The provider and `Tests.Unit`
  build clean in both configs through the same solution, and the verify command passes.

### Check results (all run by hand, foreground)
- `msbuild Preview3D.slnx /p:Configuration=Release /p:Platform=x64 /t:Preview3DThumbnailProvider`
  → exit 0, 0 warnings; DLL at `x64\Release\Preview3DThumbnailProvider.dll`.
- `msbuild Preview3D.slnx /p:Configuration=Debug /p:Platform=x64 /t:Preview3DThumbnailProvider`
  → exit 0, 0 warnings (after the `/Zi` fix).
- `x64\Release\Tests.Unit.exe` (verify command, from the repo root) → **126 cases / 74024 assertions,
  all passed**.
- `x64\Debug\Tests.Unit.exe` → 126 cases / 74112 assertions, all passed.
- `x64\Release\Tests.Unit.exe "[provider]"` and `x64\Debug\Tests.Unit.exe "[provider]"` → 3 cases /
  20 assertions passed (export surface, import closure, callable entry points).
- `pwsh -File tests\unit\check-provider-dependency-closure.ps1 -Configuration Release|Debug` → exit 0.
  Release imports `VCRUNTIME140.dll`, `api-ms-win-crt-runtime-l1-1-0.dll`, `KERNEL32.dll`; Debug
  imports `VCRUNTIME140D.dll`, `ucrtbased.dll`, `KERNEL32.dll`. No `Preview3D*` import.
- `dumpbin /exports x64\Release\Preview3DThumbnailProvider.dll` → exactly `DllCanUnloadNow`,
  `DllGetClassObject`.
- `dumpbin /headers` + `/loadconfig` (both configs): `Control Flow Guard`, `CET compatible`,
  `Dynamic base`, `NX compatible`, `High Entropy Virtual Addresses` all present; Guard CF function
  table populated.
- `msbuild Preview3D.slnx /p:Configuration=Release|Debug /p:Platform=x64 /m` → exit 1; the **only**
  errors are the two `compatibility-host-step` `C1083`s above; zero warnings; provider and `Tests.Unit`
  build in both.
- `git check-ignore -v thumbnail-provider/Preview3DThumbnailProvider.vcxproj.user
  thumbnail-provider\x64\...` → ignored by `.gitignore` `*.user` and `x64/`; both paths are untracked.
  `Preview3D.slnx` declares only the `x64` platform and the provider only `Debug|x64`/`Release|x64`.

### What the next task must know
- T11: keep the `.def` and the two `PRIVATE` exports; delete/replace `ProviderExports.cpp` bodies.
  Bear in mind the stub's `CLASS_E_CLASSNOTAVAILABLE` for a family CLSID is scaffolding — only the
  unknown-CLSID and `E_POINTER` results are stable test contract.
- T17: the frozen harness name is `Tests.ProviderHost.exe`; it should activate via
  `LoadLibrary`/`GetProcAddress` or `CoCreateInstance`, not by linking.
- Environment follow-up (pre-existing): install the OCCT `x64-windows-static-md` triplet for
  `compatibility-host-step` (or reconcile its hardcoded include path with its `x64-windows` install)
  before the full `Preview3D.slnx` build can be green.
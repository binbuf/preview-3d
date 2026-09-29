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
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
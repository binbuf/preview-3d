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
- [ ] Build Release x64 through `Preview3D.slnx`.
- [ ] Stage `Preview3DThumbnailProvider.dll` plus every non-system runtime it resolves into a scratch directory (no machine-wide install required).
- [ ] Register the implemented family's CLSID (InprocServer32, `ThreadingModel=Apartment`) and the T03-validated ShellEx handler mapping through a small registration script (the DLL exports no `DllRegisterServer`); record the exact keys and values written.
- [ ] Clear/refresh the Windows thumbnail cache, browse a real `.stl`, and confirm a model-derived thumbnail appears at default DPI.
- [ ] Confirm the handler loaded into the isolated Shell surrogate (not `explorer.exe`) and that no `DisableProcessIsolation` value is set.
- [ ] Save the staging/registration/cleanup steps as a repeatable script plus a short procedure note so every later family task re-runs it.

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
- [ ] Release build succeeds via `Preview3D.slnx`.
- [ ] A real `.stl` shows a model-derived Explorer thumbnail at default DPI.
- [ ] Evidence shows the handler in the isolated surrogate with no `DisableProcessIsolation`.
- [ ] A repeatable smoke script/procedure exists, its cleanup removes all written keys, and later tasks can reuse it.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_

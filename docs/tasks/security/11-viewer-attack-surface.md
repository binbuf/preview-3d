---
verify: x64\Release\Tests.Unit.exe "~[graphics]"
---

# T11 — Viewer local attack-surface reduction

## Goal
The trusted viewer no longer exposes an unvalidated file-write CLI, an in-process test control
surface in production, an unreachable in-process model parser, or reparse-point-following profile
writes, so a launcher or planted file cannot abuse the app as a helper.

## Context (read first)
- `interactive-viewer/src/app/Preview3D.cpp:4368-4369` parses `--benchmark-result=<path>` with no
  validation; `:515-518` opens it `CREATE_ALWAYS` and writes benchmark JSON (which embeds the
  fixture path). A shortcut/association can truncate any user-writable file or write to UNC.
- `interactive-viewer/src/app/Preview3D.cpp:3083-3104` — `WM_COPYDATA` and the `WM_APP+104` smoke
  surface accept paths/fault toggles from any local process whenever `--app-smoke` is passed;
  the flag ships in Release.
- `interactive-viewer/Preview3D.vcxproj:135` compiles `src/render/Model.cpp` (a full in-process
  GLB/JSON parser) into the trusted EXE with no call site — a latent sandbox bypass if ever wired.
  `:57` links `ws2_32.lib` with no Winsock call.
- `interactive-viewer/src/app/Settings.cpp:161-176` and `ShellIntegration.cpp:180-191` write fixed
  `.tmp` names without `FILE_FLAG_OPEN_REPARSE_POINT`; a planted symlink/hardlink redirects the
  write. (DerivedCache is test-only but has the same pattern.)
- `interactive-viewer/src/app/Preview3D.cpp:2164-2177,2200` — worker-supplied missing-asset
  references are shown verbatim; control characters can spoof dialog text.
- Tests: `tests/unit`, `tests/app-smoke`.

## Scope
- [x] Remove `--benchmark-result` from production builds or validate it to an app-owned directory
      with a fixed extension; never accept UNC/ADS/traversal. Same treatment as other developer
      flags if the benchmark lane needs to stay.
- [x] Compile the `--app-smoke`/`WM_COPYDATA`/`WM_APP+104` control surface only in developer/test
      builds (or reject the flag in Release), so shipping binaries cannot be driven by local
      processes.
- [x] Delete `src/render/Model.cpp` (and `Model.h` remnants) from `Preview3D.vcxproj`, or move it
      under the test-only project that actually uses it; remove the unused `ws2_32.lib` link.
- [x] Open temp files with `FILE_FLAG_OPEN_REPARSE_POINT`, create them with a unique name
      (`GetTempFileName`-style or GUID), and re-validate with the handle; do not write through a
      pre-existing link. Apply to settings, Open With cache, and the test-only DerivedCache.
- [x] Sanitize missing-asset dialog/UI text (strip control characters, bound length) before display.
- [x] Tests: CLI rejection for hostile `--benchmark-result`, reparse-point write resistance, and UI
      sanitization unit coverage.

## Out of scope
- Active-instance IPC trust (→ SEC-12).
- Cache manifest/checksum validation (already sound).

## Design notes
- Removing a developer flag must update `tests/app-smoke` and the qualification harness in the same
  session; record the replacement.
- Do not "clean" the path and then write it unwrapped: validate and open with the handle, then
  verify the opened handle's final path.
- `FILE_FLAG_OPEN_REPARSE_POINT` plus `CreateFileW` on a planted symlink opens the link itself;
  prefer `CREATE_NEW` with unique names and fail closed if the name exists.

## Done when
- [x] Debug and Release build; `Tests.Unit` passes with the new cases.
- [x] Release binary contains no accepted developer control flags (verified and recorded).
- [x] Hand-off filled in.

## Hand-off
**What landed**

- New `interactive-viewer/src/platform/SafeFileOps.{h,cpp}` (`preview3d::safeio`): `SanitizeDisplayText`
  (strips C0/C1/DEL + Unicode bidi/format controls, trims, bounds length), `AppDataDirectory`,
  `IsSafeOutputPath`, and `WriteFileAtomically` (GUID-named `CREATE_NEW` +
  `FILE_FLAG_OPEN_REPARSE_POINT` sibling, handle re-validation, flush, `MoveFileExW`).
- `Directory.Build.props`: `PREVIEW3D_ENABLE_TEST_CONTROL` defined only for Debug.
- `Preview3D.cpp`: `--app-smoke`/`--activation-smoke`/`--*-smoke`/`--benchmark-worker-budget-failure`
  parsing and the `WM_APP+104`/`WM_COPYDATA` bodies are gated on that macro (Release flags fall to the
  unknown-option usage exit 2; messages return 0). `--benchmark-result` is validated to
  `%LOCALAPPDATA%\Binbuf\Preview 3D\**.json` via `IsSafeOutputPath` and written with
  `WriteFileAtomically`. `BuildModelWarningText` sanitizes each missing-asset reference (256 chars).
- `Settings.cpp`, `ShellIntegration.cpp` (`SaveCache`), and `DerivedCache.cpp` (`Put`) now use
  `WriteFileAtomically` instead of a fixed `<file>.tmp` + `CREATE_ALWAYS`.
- `Model.cpp` removed from `Preview3D.vcxproj`/`.filters`; `ws2_32.lib` dropped. `TransformBounds`
  moved inline into `Model.h`; `Model.cpp` stays on disk for `tools/build-test-loader.ps1`.
- New `tests/unit/SafeFileOpsTests.cpp` (9 cases) added to `Tests.Unit.vcxproj`.
- `tests/app-smoke/run.py` rejects Release; `tests/performance/qualify.py` writes benchmark results
  under the app-owned dir (archiving a copy), and rejects `--copy-delay`/`--worker-budget-failure`
  on Release.
- Docs: `docs/design/06-application-lifecycle-and-ipc.md` and
  `docs/design/09-quality-performance-and-security.md` updated; ADR-0040 added.

**Deviations**

- `Model.h` is kept (its `ModelData`/`ModelStats` types are the viewer's load-result contract); only
  the parser TU is unlinked. "Model.h remnants" means the out-of-line `TransformBounds`/`LoadGlb`
  symbols, not the shared types.
- The benchmark lane stays in Release, so `--benchmark-result` is validated rather than removed.
  Confining it to the app-owned directory required the `qualify.py` change above.
- The app-smoke external control lane is now Debug-only. All `tests/app-smoke/*.py` default to Debug;
  only `run.py` was given an explicit Release rejection in this session. Other lanes that pass
  `--configuration Release` will now fail (the CLI flags are gone), so run them Debug.
- `--coarse-proxy-smoke` also gates off in Release because it sets `appSmoke`; `qualify.py` refuses
  `--copy-delay` on Release.

**Check results (Release unless noted)**

- `MSBuild interactive-viewer\Preview3D.vcxproj /p:Configuration=Debug|Release /p:Platform=x64`
  → both link (`x64\Debug\Preview3D.exe`, `x64\Release\Preview3D.exe`), no `ws2_32`.
- `MSBuild tests\unit\Tests.Unit.vcxproj ... Debug|Release` → both link.
- `npm test` → exit 0, **366 cases / 135098 assertions, all passed** (was 357).
- `x64\Release\Tests.Unit.exe "[security]"` and Debug → 112 assertions / 13 cases, all passed.
- Binary scan (ASCII + UTF-16LE) for the flag literals: Release `--app-smoke`,
  `--benchmark-worker-budget-failure`, `--coarse-proxy-smoke`, `--uma-budget-smoke`,
  `--progressive-smoke`, `--texture-mip-smoke`, `--queue-smoke`, `--activation-smoke` all absent;
  Debug all present (`--benchmark-result=` present in both, by design).

**Remaining / next**

- No blockers. Deriving an app-owned benchmark path in other performance scripts may be needed if
  they call the viewer directly instead of through `qualify.py`.
- T12 (active-instance IPC) is unaffected by this slice; the message-surface gate is orthogonal.
# 0040 — Viewer local attack surface: gated test control, confined benchmark output, reparse-safe writes

## Status
accepted

## Context
The audit found the trusted `Preview3D.exe` exposed several local-only ways to abuse it as a helper:
an unvalidated `--benchmark-result=<path>` opened `CREATE_ALWAYS` (a shortcut or association could
truncate any user-writable file or write to a UNC/ADS target), an `--app-smoke` control surface
(`WM_COPYDATA` path injection and the `WM_APP+104` query/command/fault handler) that any local process
could drive and that shipped in Release, an unreachable full GLB/JSON parser (`src/render/Model.cpp`)
compiled into the trusted EXE with no call site, an unused `ws2_32.lib` link, fixed-name `<file>.tmp`
profile writes that follow a planted reparse point, and worker-supplied missing-asset references shown
verbatim with no control-character filtering.

## Decision
- **Developer control is compiled out of Release.** A new `PREVIEW3D_ENABLE_TEST_CONTROL` macro is
  defined only for Debug in `Directory.Build.props`. It gates parsing of `--app-smoke`,
  `--activation-smoke`, the other `--*-smoke` switches, and `--benchmark-worker-budget-failure`, and
  makes the `WM_APP+104` and `WM_COPYDATA` handlers return 0 early. In Release those flags reach the
  unknown-option usage exit (code 2); the message handlers are inert even if posted.
- **Benchmark output is confined and validated.** `--benchmark*` stays in Release (the
  performance-qualification lane), but a non-empty `--benchmark-result` must pass
  `safeio::IsSafeOutputPath(path, L".json", AppDataDirectory())`: absolute drive path, no UNC/device
  prefix, no extra colon/ADS, no `.`/`..` traversal, no control characters, `.json` extension, and
  under `%LOCALAPPDATA%\Binbuf\Preview 3D`. The benchmark block is the only writer.
- **One shared reparse-safe writer.** `safeio::WriteFileAtomically` (header/impl in
  `interactive-viewer/src/platform/SafeFileOps.{h,cpp}`) creates a GUID-named sibling with `CREATE_NEW`
  + `FILE_FLAG_OPEN_REPARSE_POINT`, verifies the opened handle is not a reparse point, writes, flushes,
  and `MoveFileExW`s over the destination. Settings, the Open With cache, and the test-only
  DerivedCache all use it instead of a fixed `<file>.tmp` + `CREATE_ALWAYS`.
- **The in-process parser leaves the trusted EXE.** `src/render/Model.cpp` is removed from
  `Preview3D.vcxproj` (and `ws2_32.lib` from its link line). The only symbol the viewer still needs,
  `TransformBounds`, moves inline into `Model.h`; `Model.cpp` remains on disk for the manual
  `tools/build-test-loader.ps1` loader tool.
- **Display text is sanitized.** `safeio::SanitizeDisplayText` strips C0/C1/DEL and Unicode
  bidi/format controls, trims, and bounds length; `BuildModelWarningText` applies it to each
  worker-supplied missing-asset reference before it reaches the dialog.

## Consequences
- Release `Preview3D.exe` contains none of the developer flag literals; Debug retains them (verified
  by binary search and recorded in the T11 hand-off). `tests/app-smoke/run.py` and
  `tests/performance/qualify.py` were updated: app-smoke requires Debug; benchmark copy-delay /
  worker-budget-failure require Debug; benchmark results are read from the app-owned directory.
- `SafeFileOpsTests` covers sanitization (controls/bidi/length), hostile `--benchmark-result`
  rejection, atomic round-trip/replace/refuse-existing, planted `.tmp` symlink resistance, symlinked
  destination replacement, and a spawned-viewer CLI rejection (exit 64).
- `Tests.Unit` Release 366 cases green (was 357); the viewer builds Debug and Release.
- Rejected: keeping a runtime secret/registration for app-smoke (defeats the point on a local-host
  attacker); allowing `--benchmark-result` anywhere with only lexical validation (a file association
  would still be a truncation primitive); deleting `Model.h` entirely (its `ModelData`/`ModelStats`
  types are the viewer's load-result contract).
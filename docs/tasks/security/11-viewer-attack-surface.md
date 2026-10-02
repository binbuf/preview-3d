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
- [ ] Remove `--benchmark-result` from production builds or validate it to an app-owned directory
      with a fixed extension; never accept UNC/ADS/traversal. Same treatment as other developer
      flags if the benchmark lane needs to stay.
- [ ] Compile the `--app-smoke`/`WM_COPYDATA`/`WM_APP+104` control surface only in developer/test
      builds (or reject the flag in Release), so shipping binaries cannot be driven by local
      processes.
- [ ] Delete `src/render/Model.cpp` (and `Model.h` remnants) from `Preview3D.vcxproj`, or move it
      under the test-only project that actually uses it; remove the unused `ws2_32.lib` link.
- [ ] Open temp files with `FILE_FLAG_OPEN_REPARSE_POINT`, create them with a unique name
      (`GetTempFileName`-style or GUID), and re-validate with the handle; do not write through a
      pre-existing link. Apply to settings, Open With cache, and the test-only DerivedCache.
- [ ] Sanitize missing-asset dialog/UI text (strip control characters, bound length) before display.
- [ ] Tests: CLI rejection for hostile `--benchmark-result`, reparse-point write resistance, and UI
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
- [ ] Debug and Release build; `Tests.Unit` passes with the new cases.
- [ ] Release binary contains no accepted developer control flags (verified and recorded).
- [ ] Hand-off filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
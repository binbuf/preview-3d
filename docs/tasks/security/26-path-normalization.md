---
verify: x64\Release\Tests.ImportIsolation.exe
---

# T26 — Normalize untrusted paths before opening (remote UNC + absolute primary)

## Goal
A crafted path cannot make the trusted viewer or broker initiate an SMB/device open before
rejection, and the primary source path must be absolute/drive-qualified like the sidecar resolver
already requires.

## Context (read first)
- Follow-up audit evidence: `interactive-viewer/src/app/Preview3D.cpp:1816` rejects only `\\`-prefixed
  paths unless they start `\\?\`; `shared/import-broker/src/SourceFileAccess.cpp:38` has the same
  shape. A forward-slash UNC (`//server/share/model.glb`) and `\\?\UNC\server\...` pass the pre-open
  checks, `CreateFileW` initiates the connection, and only the *post-open* canonical check
  (`SourceFileAccess.cpp:75`) rejects it.
- `SourceFileAccess.cpp:18-42` does not require the primary path to be absolute/drive-qualified, so
  a relative `foo\bar` resolves against the process CWD, unlike `SidecarPathResolver`.
- `docs/tasks/security/11-viewer-attack-surface.md`, `04-sidecar-reference-validation.md`.

## Scope
- [x] Reject forward-slash UNC (`//`, `\\`, `\\?\UNC\`) and device paths *before* `CreateFileW` in
      both the viewer guard and `OpenAndCanonicalizeSourceFile`; normalize before classification.
- [x] Require an absolute/drive-qualified primary path (or normalize to one) and keep the canonical
      post-open containment check as defense in depth.
- [x] Tests: viewer `BeginOpen`/`SafeFileOps` cases for `//server/...`, `\\?\UNC\...`, forward
      slashes and a relative primary path; broker `SourceFileAccessTests` cases.

## Out of scope
- Sidecar reference validation (done, SEC-04); archive-entry policy (done, SEC-05).

## Design notes
- Reject, do not sanitize; a rejected path must never reach `CreateFileW`.
- Keep the existing post-open canonical containment check.
- The payload is unsigned until SEC-14; do not add a signer check here.

## Done when
- [x] `x64\Release\Tests.ImportIsolation.exe` passes with the new broker cases.
- [x] `x64\Release\Tests.Unit.exe "[security]"` passes with the new viewer cases.
- [x] Hand-off filled in.

## Hand-off

**What landed.**
- **Shared classifier.** New header-only
  `shared/platform/include/platform/SourcePathPolicy.h` exposes
  `platform::ClassifySourcePath` returning `LocalAbsolute | RemoteOrDevice | Relative` and
  `platform::NormalizeSourcePathSeparators`. It converts `/` to `\` on a copy and classifies that
  copy, so `//server/share` is read as a UNC; the original text is what gets opened.
  `LocalAbsolute` is `X:\...` or `\\?\X:\...`; `RemoteOrDevice` covers `\\server\share`,
  `//server/share`, `\\.\...`, `\\?\UNC\...`, `\\?\GLOBALROOT\...`, `\\?\Volume{...}\`;
  everything else (bare relative, `\foo`, `C:foo`) is `Relative`.
- **Broker (`shared/import-broker/src/SourceFileAccess.cpp`).** `OpenAndCanonicalizeSourceFile`
  now classifies first and returns `UnsafeReference` for `RemoteOrDevice` and for `Relative`, before
  `CreateFileW`. The ADS/extra-colon check and the `GetDriveTypeW == DRIVE_REMOTE` mapped-drive probe
  are retained; the old prefix-only checks were removed.
- **Viewer (`interactive-viewer/src/app/Preview3D.cpp`).** `BeginOpen`'s guard now rejects anything
  that is not `platform::SourcePathKind::LocalAbsolute` (same `failure.remotePath` surface). This
  catches `//server/...` and `\\?\UNC\...`, which previously reached the import path.
- **Tests.** `tests/import-isolation/SourceFileAccessTests.cpp`: rejects forward-/back-slash UNC,
  `\\?\UNC\`, `\\.\`, `\\?\GLOBALROOT\`, and relative/drive-relative primaries; accepts a
  forward-slash absolute local path (proves the separator normalization does not over-reject).
  `tests/unit/SafeFileOpsTests.cpp`: `[security][source-path]` cases for the same classification the
  viewer guard calls.
- **Docs.** ADR-0053 (new); `docs/design/03-file-formats-and-ingestion.md` Input boundary and path-
  authority paragraph; `docs/design/09-quality-performance-and-security.md` files-and-archives list;
  `docs/design/07-user-experience.md` open-format paragraph.

**Deviations.**
- `BeginOpen` is in an anonymous namespace in `Preview3D.cpp`, so the viewer tests exercise
  `platform::ClassifySourcePath` (the exact function the guard calls, `[security]`-tagged) rather
  than `BeginOpen` itself.
- The relative-path rejection reuses the existing `failure.remotePath` string instead of adding a
  localization key; a shell/drop never produces a relative primary, and the shared helper has no
  message table. Keeps `generate-language-packs.ps1 -Verify` green.
- Added `ClInclude` entries for the new header to the three consuming `.vcxproj` files (no source
  list change; the header is header-only).

**Check results (Release x64).**
- `msbuild tests\import-isolation\Tests.ImportIsolation.vcxproj` — exit 0.
- `x64\Release\Tests.ImportIsolation.exe` — 403 passed / 5 skipped, 293103 assertions, exit 0.
- `x64\Release\Tests.ImportIsolation.exe "OpenAndCanonicalizeSourceFile*"` — 27 assertions / 7 cases,
  exit 0.
- `msbuild tests\unit\Tests.Unit.vcxproj` — exit 0; `x64\Release\Tests.Unit.exe "[security]"` —
  142 assertions / 20 cases, exit 0.
- `msbuild interactive-viewer\Preview3D.vcxproj` — exit 0.

**Remaining work / blockers.** None.
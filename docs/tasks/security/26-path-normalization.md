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
- [ ] Reject forward-slash UNC (`//`, `\\`, `\\?\UNC\`) and device paths *before* `CreateFileW` in
      both the viewer guard and `OpenAndCanonicalizeSourceFile`; normalize before classification.
- [ ] Require an absolute/drive-qualified primary path (or normalize to one) and keep the canonical
      post-open containment check as defense in depth.
- [ ] Tests: viewer `BeginOpen`/`SafeFileOps` cases for `//server/...`, `\\?\UNC\...`, forward
      slashes and a relative primary path; broker `SourceFileAccessTests` cases.

## Out of scope
- Sidecar reference validation (done, SEC-04); archive-entry policy (done, SEC-05).

## Design notes
- Reject, do not sanitize; a rejected path must never reach `CreateFileW`.
- Keep the existing post-open canonical containment check.
- The payload is unsigned until SEC-14; do not add a signer check here.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe` passes with the new broker cases.
- [ ] `x64\Release\Tests.Unit.exe "[security]"` passes with the new viewer cases.
- [ ] Hand-off filled in.

## Hand-off
_(filled in by the implementing session)_
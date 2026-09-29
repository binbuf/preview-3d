---
verify: x64\Release\Tests.Unit.exe
---
# T24 — Implement the OBJ thumbnail adapter

## Goal
Render `.obj` geometry in Explorer with generated normals and object/group separation, while never
resolving its `.mtl` or texture sidecars — the documented stream-only behavior.

## Context (read first)
- `docs/design/03-file-formats-and-ingestion.md` — OBJ/MTL section: the thumbnail path does not resolve sidecars.
- `docs/design/05-thumbnail-provider.md` — stream ingestion and the OBJ rule.
- `docs/design/adapters/fbx-008-thumbnail.md` — ufbx provider-local configuration (shared with FBX).
- `import-worker/src/` OBJ/ufbx usage — the viewer policy reference.
- `docs/tasks/17-provider-test-harness.md`.

## Scope
- [ ] Route `.obj` to this adapter via its fixed CLSID; parse through the provider-local pinned ufbx copy with path/external-file access disabled.
- [ ] Triangulate polygons, generate/smooth normals, carry UVs/vertex colors, and separate object/group meshes.
- [ ] Ignore `mtllib` and every texture reference: render the neutral material; an MTL/texture dependency must never fail an otherwise valid mesh or trigger an open.
- [ ] Add fixtures and goldens: plain mesh, polygon/quad mesh, object-group mesh, colored mesh, and an OBJ that references an MTL/texture (must still render neutrally with no filesystem access).

## Out of scope
- MTL/material factor and texture mapping (viewer-only).
- FBX parsing (→ T31), even though both use ufbx.

## Design notes
- The provider-local ufbx copy must have path, geometry-cache, plug-in, script and environment-codec access disabled.
- No filesystem location is available to the provider; do not attempt sidecar recovery.

## Done when
- [ ] `x64\Release\Tests.Unit.exe` (the `verify:` command) passes in Debug and Release, including the MTL-referencing case with no path access.
- [ ] The isolation test confirms no file open occurs for OBJ.
- [ ] The family is smoke-checked in Explorer using the T22 procedure.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
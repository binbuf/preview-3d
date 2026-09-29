---
verify: x64\Release\Tests.Unit.exe
---
# T31 — Implement the FBX thumbnail adapter

## Goal
Render binary and ASCII `.fbx` models in Explorer: stream-contained geometry, embedded textures, and
a deterministic static start pose with supported skin/blend baked, under provider limits.

## Context (read first)
- `docs/design/adapters/fbx-008-thumbnail.md` — the full FBX thumbnail requirement.
- `docs/design/05-thumbnail-provider.md` — stream ingestion (FBX) and limits.
- `docs/legacy/fbx/FBX-001-SPIKE-RESULTS.md` — ufbx provider policy and the lack of an evaluation progress callback.
- `import-worker/src/` FBX/ufbx usage — viewer policy reference.
- `docs/tasks/17-provider-test-harness.md`.

## Scope
- [ ] Route `.fbx` to this adapter via its fixed CLSID `{FBC218D4-FD2C-41DF-B168-7F3B9E53C84E}`; parse with provider-local ufbx (path/external-file, cache, plug-in, script and environment-codec access disabled).
- [ ] Preflight element/triangle counts before evaluation; apply the deterministic static pose and supported skin/blend policy for fixtures small enough to evaluate within budget.
- [ ] Decode only embedded allowlisted images; external textures use the neutral fallback and external required geometry returns the generic icon.
- [ ] Feed finite evaluated triangles and material colors to the shared sampler/rasterizer.
- [ ] Add fixtures and goldens: binary and ASCII FBX, static pose, embedded texture, external-dependency fallback, malformed/over-limit/deadline/OOM.

## Out of scope
- Scene/instance viewer contract work and FBX-007 qualification (viewer program).
- Dynamic constraints, NURBS, subdivision, animation playback.

## Design notes
- ufbx evaluation has no progress callback: encode a conservative size/deadline policy; never allow an unbounded library call just because Shell uses a surrogate.
- Constrain ufbx allocation/temp-memory caps explicitly.
- The DLL does not launch or IPC to any product process.

## Done when
- [ ] `x64\Release\Tests.Unit.exe` (the `verify:` command) passes in Debug and Release for binary/ASCII, pose, embedded texture and fallbacks.
- [ ] Deadline/OOM/over-limit cases fail safe with no Explorer hang.
- [ ] The family is smoke-checked in Explorer using the T22 procedure.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
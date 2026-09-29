---
verify: x64\Release\Tests.Unit.exe
---
# T23 — Implement the PLY thumbnail adapter

## Goal
Render ASCII and binary (both endian) `.ply` files in Explorer, for triangle meshes and point clouds,
including vertex colors, under provider limits.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — point-cloud splat rendering and sampling.
- `docs/design/03-file-formats-and-ingestion.md` — PLY subset (meshes/points, both endian).
- `import-worker/src/PlyAdapter.*` — the viewer parser to reuse as source (see ADR-0004).
- `docs/tasks/14-geometry-sampler.md`, `docs/tasks/15-cpu-tile-rasterizer.md`, `docs/tasks/17-provider-test-harness.md`.

## Scope
- [ ] Route `.ply` to this adapter via its fixed CLSID; parse ASCII and little/big-endian binary.
- [ ] Support triangle meshes and point clouds; carry vertex colors when present; emit product-owned geometry.
- [ ] Bound unknown-property/list skipping, element counts and per-face vertex counts; never require all source positions in private memory.
- [ ] Feed point clouds to the depth-tested round-splat path and meshes to the triangle path.
- [ ] Add fixtures and goldens: ASCII/binary mesh, little/big-endian point cloud, colored points, unknown properties/lists, hostile element count, truncation — the last three asserting typed failures.

## Out of scope
- STL parsing (→ T21).
- Viewer Tier-A PLY proxy/LOD qualification (viewer program).

## Design notes
- Point-only PLY must produce a usable thumbnail, not a fallback.
- Use deterministic reservoir sampling for points and verify bounds incrementally.
- Variable-length face lists need a bounded offset/scan policy; do not trust declared counts without checked reads.

## Done when
- [ ] `x64\Release\Tests.Unit.exe` (the `verify:` command) passes in Debug and Release across encodings, meshes and clouds.
- [ ] Hostile list/count inputs fail safely before allocation.
- [ ] The family is smoke-checked in Explorer using the T22 procedure.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
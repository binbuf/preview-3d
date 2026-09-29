---
verify: x64\Release\Tests.Unit.exe
---
# T21 — Implement the STL thumbnail adapter

## Goal
Render ASCII and binary `.stl` models in Explorer using the product STL parser under provider limits,
with neutral shading.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — stream ingestion (STL is fully stream-contained).
- `docs/design/03-file-formats-and-ingestion.md` — STL subset and budget column.
- `import-worker/src/StlAdapter.*` — the viewer parser to reuse as source (see ADR-0004).
- `docs/tasks/14-geometry-sampler.md`, `docs/tasks/17-provider-test-harness.md`.

## Scope
- [ ] Route `.stl` to this adapter via its fixed CLSID; detect ASCII vs binary on the stream.
- [ ] Emit bounded triangles with computed/neutral normals and the neutral material.
- [ ] Reuse the checked triangle-count and record bounds so an adversary cannot force an unbounded read or allocation; split/stream the read against the deadline.
- [ ] Add fixtures and goldens: ASCII mesh, binary mesh, large triangle-count input, truncated file, fabricated count, non-finite vertices — the last four asserting typed failures.

## Out of scope
- PLY parsing (→ T23), regardless of shared point/mesh paths.
- Viewer STL streaming/LOD behavior (viewer program).

## Design notes
- STL is stream-contained and needs no sidecar policy.
- Prefer a single bounded pass with the sampler able to stop early at the inspect cap.
- Keep the ASCII tokenizer bounded and locale-independent.

## Done when
- [ ] `x64\Release\Tests.Unit.exe` (the `verify:` command) passes in Debug and Release for ASCII/binary and every malformed case.
- [ ] A fabricated-count input is rejected before allocation.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
---
verify: x64\Release\Tests.Unit.exe
---
# T14 — Implement the deterministic geometry sampler

## Goal
Reduce an arbitrarily large inspected mesh/point set to a small deterministic representative set that
preserves silhouette, materials and disconnected components, within the provider's sample caps.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — "Geometry sampling".
- `docs/design/03-file-formats-and-ingestion.md` — inspected/sampled caps and coverage expectations.
- `docs/design/11-decisions-and-risks.md` — ADR-015 (bounded coarse/full sampling and the coverage floor), the policy this sampler mirrors in miniature.
- `docs/tasks/06-budgets-deadlines-hresults.md` — the inspected/rasterized caps.

## Scope
- [ ] Accumulate finite bounds; discard degenerate/non-finite triangles and points.
- [ ] Inspect at most 2 M triangles / 6 M points and admit at most 250 k representatives to rasterization; charge retained sample/scratch memory against the T06 aggregate budget ledger.
- [ ] Use a deterministic spatial/reservoir sampler with a stable source-derived seed so output is identical across runs and Explorer cache-consistent.
- [ ] When the source exceeds the inspect cap, place a deterministic stratified sample across the whole declared extent for seek-capable streams (never a source prefix); a non-seekable over-cap source without trustworthy metadata returns the safe fallback rather than a biased prefix.
- [ ] Guarantee minimum representation for material boundaries and large/disconnected components; support a coverage floor of one primitive per nonempty region when caps permit.
- [ ] Add tests: reordered input yields the same selection, component coverage, deterministic bytes, hostile counts and NaN/Inf rejection.

## Out of scope
- Per-format geometry extraction (→ T21–T34).
- GPU/LOD simplification (meshoptimizer LOD is a viewer concern, not the provider).

## Design notes
- Point clouds use the same deterministic spatial/reservoir policy as meshes.
- The sampler must never require the whole source in private memory.
- Sampling speed matters against the deadline; keep it allocation-lean and single-pass where possible.

## Done when
- [ ] Sampler tests pass in Debug and Release, including reordering determinism and coverage.
- [ ] A multi-million-primitive input stays within the inspected/sampled caps and the deadline.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
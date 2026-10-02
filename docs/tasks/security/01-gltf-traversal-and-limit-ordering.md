# T01 — Bound glTF traversal and fix worker limit ordering

## Goal
A hostile glTF/GLB can no longer multiply work exponentially through shared node subtrees, and
per-format caps are checked before the allocation or expansion they are meant to bound, so a
malformed file inside the sandbox cannot pin CPU or allocate on the broker's behalf.

## Context (read first)
- `import-worker/src/GltfAdapter.cpp:1986` — `VisitNode` caps only depth (`kMaxNodeDepth = 256`,
  `:53`) and detects true cycles, but a DAG diamond is re-walked: ~60 nodes with children
  `[i+1, i+1]` is 2^60 visits. `ConvertPrimitive`'s occurrence cap (`:1186-1250`) never runs for
  mesh-less nodes, and the walk has no cancellation check.
- `import-worker/src/UsdAdapter.cpp:617` — `expanded.reserve(triangleIndices.size() * stride)`
  runs in `RecoverMissingTexcoords` before the Tier B triangle cap at `:1098` rejects the mesh, so
  a large-USDA mesh can reserve hundreds of MB before admission.
- `docs/design/03-file-formats-and-ingestion.md` — Tier A/B limits this task must keep true.
- `tests/import-isolation/GltfImportTests.cpp`, `tests/import-isolation/UsdSpikeTests.cpp` — where
  the regression cases belong.

## Scope
- [ ] Cap total `VisitNode` visits per walk (reuse an existing Tier A object/budget constant; do not
      invent a new public limit without an ADR) or memoize subtrees already `visitState == 2`.
- [ ] Add cancellation checks inside the node walk and any other adapter loop that currently checks
      only between top-level stages.
- [ ] Move the USD UV expansion behind (or bound it by) the triangle/vertex caps; use checked
      multiplication for the reserve size.
- [ ] Regression tests: deep diamond DAG (mesh-less and mesh-bearing), oversized USDU mesh with a UV
      primvar; assert typed failure and bounded wall time, not a crash.
- [ ] Sweep the other adapters for the same "cap after expansion" pattern and fix or file a
      follow-up task (record which).

## Out of scope
- Third-party decoder allocation ordering (→ SEC-02).
- Broader fuzzing (→ SEC-16); this task lands unit/regression cases only.

## Design notes
- Prefer a total-visit counter over per-node memoization if it is simpler to make deterministic; the
  walk must stay deterministic for a given input.
- A limit hit is `ResourceLimit`; a cycle stays `MalformedData`. Do not conflate them.
- Keep `kMaxNodeDepth` as a depth guard, not the only guard.

## Done when
- [ ] `Tests.ImportIsolation` passes in Debug and Release with the new cases.
- [ ] A hand-built 40-level doubling DAG fails fast with `ResourceLimit` (record the measured time).
- [ ] No design limit changed without an ADR; if one changed, link it.
- [ ] Hand-off filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
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
- [~] `Tests.ImportIsolation` passes in Debug and Release with the new cases. (New cases pass in both
  configs; 13 Debug / 11 Release failures are pre-existing and unrelated — see Hand-off.)
- [x] A hand-built 40-level doubling DAG fails fast with `ResourceLimit` (Debug 321 ms mesh-less /
  37 ms mesh-bearing; Release 45 ms / 26 ms).
- [x] No design limit changed without an ADR; if one changed, link it. (No change.)
- [x] Hand-off filled in.

## Hand-off

### What landed
- `import-worker/src/GltfAdapter.cpp`
  - `WalkState::totalNodeVisits` (uint64) counts every `VisitNode` invocation for the current
    walk. `VisitNode` now fails with `ResourceLimit` once it exceeds `kTierAObjectLimit`
    (100000, the existing Tier A node/object budget) — reusing an existing constant, so no new
    public limit and no ADR. A true cycle still returns `MalformedData` first.
  - `VisitNode` also checks `textureOptions.Cancelled()` at entry, so a huge-but-legal walk can be
    stopped inside the recursion rather than only between top-level stages.
  - The counter is reset after the preview-counting walk (`GltfAdapter.cpp:2576-2578` area) so the
    main walk gets its own budget.
- `import-worker/src/UsdAdapter.cpp` / `UsdAdapter.h`
  - `ExpandPrimvarForTriangulatedMesh` now applies the Tier B triangle/vertex caps *before* it
    allocates, via the new header inline `PrimvarExpansionLimit(corners)`
    (`UsdAdapter.h`), and sizes the reserve with `platform::CheckedMultiply`. A cap hit returns
    `ResourceLimit`; a wrapped size returns `MalformedData`.
  - `ReadTexcoordPrimvar` sizes its buffer with `CheckedMultiply` too.
  - `RecoverMissingTexcoords` now returns a typed `ImportErrorCode` and checks cancellation per
    mesh; it is called before the later `scene.meshes.size() > kTierBObjectLimit` check, so this
    ordering was the cited bug.
  - `FlattenNodes` gained a depth cap (`kUsdMaxNodeDepth = 256`, matching the protocol-v10
    hierarchy ceiling in docs/design/03) and a `kTierBObjectLimit` node cap plus cancellation, so a
    hostile deep/broad USD node tree cannot overflow the stack or grow an unbounded flat list.
- Tests: `GltfImportTests.cpp` adds two `[dag]` cases (a 40-level doubling DAG, mesh-less and
  mesh-bearing) asserting `ResourceLimit` and <5s wall time; `UsdSpikeTests.cpp` adds a boundary
  unit test for `PrimvarExpansionLimit`.

### Check results
- Debug: `x64\Debug\Tests.ImportIsolation.exe "[dag]"` -> 2/2 passed; mesh-less 321 ms,
  mesh-bearing 37 ms.
- Release: `x64\Release\Tests.ImportIsolation.exe "[dag]"` -> 2/2 passed; mesh-less 45 ms,
  mesh-bearing 26 ms.
- New USD boundary case passes Debug and Release.
- Full suite, Debug: 364/377 passed, 13 failed; Release: 366/377 passed, 11 failed. Every failure
  is pre-existing and unrelated to this task (checked-in USD fixture SHA drift in
  `UsdProtocolTests.cpp:1141/1230` and `UsdSpikeTests.cpp:97`; `FbxImportTests.cpp:276`;
  `SidecarPathResolverTests.cpp` user-asset-root cases; `ThreeMfSpikeTests.cpp:630`;
  `UsdSpikeTests.cpp:230` low-Job-cap spike). None of the new/changed code paths is involved.
- No design limit changed, so no ADR.

### Deviations
- The task asked for an "oversized USDU mesh with a UV primvar" sandbox case asserting typed
  failure. A real oversized mesh must exceed the Tier B vertex cap (60M corners / 20M triangles),
  which is a ~150-200 MB USDA fixture and parses for tens of seconds — impractical for the
  always-on suite and at risk of the 120 s worker reply timeout. Instead the cap boundary is
  covered directly by a cheap unit test over the new `PrimvarExpansionLimit` helper (exact-cap
  allowed; cap+1 triangle and cap+1 corner rejected as `ResourceLimit`), and the checked
  multiplication and ordering are exercised by the existing USD sandbox fixtures. A large
  qualification fixture remains possible follow-up work.

### Sweep (cap-after-expansion) result
- Reviewed `GltfAdapter`, `FbxAdapter`, `ObjAdapter`, `PlyAdapter`, `StlAdapter`,
  `ThreeMfAdapter`. All reserve/resize calls are bounded by a cap checked earlier in the same
  function (e.g. PLY's `vertexBudget` is derived from `TierBScratchLimit()` before
  `meshVertices.reserve`/`meshIndices.reserve`; FBX/3MF reserve per bounded chunk). USD was the
  only adapter that allocated before the admitting cap. No follow-up task required from this
  sweep; PLY's `count * 3` reserve is safe because `count` is already under the vertex budget, but
  a future cleanup could switch it to `CheckedMultiply` for consistency.

### For the next task (SEC-02 / SEC-16)
- `kTierAObjectLimit` is now the per-walk glTF node-visit budget; if a legitimate very large DAG
  ever needs more, that is a deliberate limit change and needs an ADR.
- `PrimvarExpansionLimit` lives in `UsdAdapter.h` and is the single place to update if the Tier B
  caps move.
- The full `Tests.ImportIsolation` suite is not green at baseline (13 Debug / 11 Release failures
  above); do not attribute them to SEC-01.
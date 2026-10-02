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
- [x] Cap total `VisitNode` visits per walk (reuse an existing Tier A object/budget constant; do not
      invent a new public limit without an ADR) or memoize subtrees already `visitState == 2`.
- [x] Add cancellation checks inside the node walk and any other adapter loop that currently checks
      only between top-level stages.
- [x] Move the USD UV expansion behind (or bound it by) the triangle/vertex caps; use checked
      multiplication for the reserve size.
- [x] Regression tests: deep diamond DAG (mesh-less and mesh-bearing), oversized USDU mesh with a UV
      primvar; assert typed failure and bounded wall time, not a crash.
- [x] Sweep the other adapters for the same "cap after expansion" pattern and fix or file a
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
- [x] `npm test` (harness verify: `x64\Release\Tests.Unit.exe`) passes green —
  "All tests passed (134916 assertions in 339 test cases)", exit 0.
- [~] `Tests.ImportIsolation` new `[dag]` cases pass in Debug and Release. The suite now has only
  5 Debug / 3 Release failures (down from 13/11); all remaining ones are pre-existing and unrelated
  (`SidecarPathResolverTests` user-asset-root cases, `ThreeMfSpikeTests.cpp:630` missing
  problem-model, `UsdSpikeTests.cpp:230` low Job cap) — see Hand-off.
- [x] A hand-built 40-level doubling DAG fails fast with `ResourceLimit` (Debug 321 ms mesh-less /
  37 ms mesh-bearing; Release 45 ms / 26 ms).
- [x] No design limit changed without an ADR; if one changed, link it. (No change.)
- [x] Hand-off filled in.

## Hand-off

### Attempt-2 finding: the real verify blocker was fixture line endings, not SEC-01
The harness verify (`npm test` = `x64\Release\Tests.Unit.exe`) failed with exit 42 on
`tests/unit/FixtureManifestTests.cpp:52` — 14 SHA mismatches, exactly the text fixtures under
`interactive-viewer/test-assets/corpus` (`.gltf`, the two text `.ply`, `manifest.json`); every
binary fixture matched. Root cause: the repo had no `.gitattributes`, and this machine's
`core.autocrlf=true` rewrote the checked-out text fixtures to CRLF while both the committed blobs
and `Expectations.h`/`manifest.json` are LF. `tests/fixtures/generate.py` writes LF with
`write_bytes`, so the manifest is LF by construction.
The same bug explains most of what the first attempt recorded as "pre-existing USD fixture SHA
drift": `tests/fixtures/usd-spike/*.usda` were also checked out as CRLF, and their LF bytes hash to
the manifest values exactly (e.g. `compat-sub.usda` -> `a11a3798…`). So this was one repo defect,
not two: the repo had no `.gitattributes` and `core.autocrlf=true` broke every byte-exact fixture
assertion on Windows. It is independent of the SEC-01 code changes, which were already correct.
Fix: added `.gitattributes` (`interactive-viewer/test-assets/** -text` and `tests/fixtures/** -text`)
so byte-hashed assets are never line-ending converted, then re-checked-out those trees to restore
LF. `npm test` is now green, and `Tests.ImportIsolation` improved from 13 Debug / 11 Release
failures to 5 / 3. The remaining failures are genuinely unrelated (see below). No SEC-01 limit
changed, so no limit ADR; the line-ending policy is recorded in
`docs/design/adr/0029-fixture-assets-are-byte-exact.md`.

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
- Verify (Release, attempt 2): `npm test` (`x64\Release\Tests.Unit.exe`) -> "All tests passed
  (134916 assertions in 339 test cases)", exit 0. Before the `.gitattributes` fix this exited 42
  on the 14 CRLF fixture mismatches.
- Fixtures, Debug: `x64\Debug\Tests.Unit.exe "[fixtures]"` -> 1/1 case, 297 assertions, exit 0.
- Debug: `x64\Debug\Tests.ImportIsolation.exe "[dag]"` -> 2/2 passed; mesh-less 278 ms,
  mesh-bearing 34 ms.
- Release: `x64\Release\Tests.ImportIsolation.exe "[dag]"` -> 2/2 passed; mesh-less 45 ms,
  mesh-bearing 26 ms.
- New USD boundary case passes Debug and Release: `Tests[resource-limit]` (2 DAG + 1 USD case)
  -> 27 assertions, 3/3 cases, exit 0 in both configs.
- Full `Tests.ImportIsolation`, after the `.gitattributes` fix — Debug: 372/377 passed, 5 failed;
  Release: 374/377 passed, 3 failed (was 13 / 11 before the fix; the difference was the CRLF USD
  fixture hashes above, not real drift). The remaining failures are pre-existing and unrelated:
  `SidecarPathResolverTests.cpp` user-asset-root cases (Debug 920/929/950, Release 489/504/518),
  `ThreeMfSpikeTests.cpp:630` (absent problem-model file), and `UsdSpikeTests.cpp:230` (low Job
  commit cap). None involve glTF traversal or the USD adapter code changed here.
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

### Docs changed
- `docs/tasks/security/01-gltf-traversal-and-limit-ordering.md` (this hand-off).
- `docs/security/PROGRESS.md` (T01 reusable facts + fixture-line-ending gotcha).
- `docs/design/adr/0029-fixture-assets-are-byte-exact.md` (new; pins fixture bytes against EOL
  conversion). No Tier A/B limit changed, so no limit ADR.
- `.gitattributes` (new; `interactive-viewer/test-assets/** -text` and `tests/fixtures/** -text`).

### For the next task (SEC-02 / SEC-16)
- `kTierAObjectLimit` is now the per-walk glTF node-visit budget; if a legitimate very large DAG
  ever needs more, that is a deliberate limit change and needs an ADR.
- `PrimvarExpansionLimit` lives in `UsdAdapter.h` and is the single place to update if the Tier B
  caps move.
- The full `Tests.ImportIsolation` suite is not green at baseline (13 Debug / 11 Release failures
  above); do not attribute them to SEC-01. This is separate from verify, which is `npm test`
  (`Tests.Unit.exe`) and is now green.
- Keep `.gitattributes` and ADR 0029: any new byte-hashed fixture tree needs the same `-text` rule.
---
verify: x64\Release\Tests.ImportIsolation.exe
---

# T21 — Complete the SEC-01 sweep: worker USD primvar cap and provider glTF visit cap

## Goal
The two traversal/allocation-ordering gaps the SEC-01 sweep missed are closed: a USD primvar cannot
allocate from its own sample count before the mesh-keyed Tier B cap, and the provider glTF node walk
is bounded by a total-visit budget as well as the deadline.

## Context (read first)
- `docs/tasks/security/01-gltf-traversal-and-limit-ordering.md` — the class and the worker fix.
- `import-worker/src/UsdAdapter.cpp:697-728` `ReadTexcoordPrimvar` resizes `attribute.data` from
  `values.size()` with only `platform::CheckedMultiply` (overflow, not size). It is called at
  `:751` *before* `ExpandPrimvarForTriangulatedMesh` applies `PrimvarExpansionLimit(corners)`
  (`:617-622`), and that cap keys on the mesh corner count, not the primvar sample count, so a
  small mesh with a huge face-varying/Varying primvar allocates uncapped.
  `RecoverMissingTexcoords` also runs before the scene mesh-count cap (`:1499-1503`).
- `thumbnail-provider/GltfFamilyAdapter.cpp:904-934` walks glTF nodes with a depth cap
  (`kMaxTransformDepth = 256`) and a deadline checkpoint but only increments `visited` for the
  checkpoint; a mesh-less DAG re-walks shared subtrees until the deadline. The worker's `VisitNode`
  has `WalkState::totalNodeVisits` capped at `kTierAObjectLimit`.

## Scope
- [x] Bound the USD primvar buffer by an explicit limit *before* `attribute.data.resize`; reuse a
      Tier B constant where possible, otherwise add one with an ADR.
- [x] Add a total-visit cap to the provider glTF walk mirroring `kTierAObjectLimit` (or a provider
      constant), failing `ErrorCode::ResourceLimit`; keep the depth cap and deadline.
- [x] Regression tests: a USD mesh whose primvar count vastly exceeds its corners is rejected
      without a large allocation; a provider mesh-less doubling DAG fails fast with `ResourceLimit`.
- [x] Record any new public/Tier limit in an ADR; if an existing constant is reused, note that.

## Out of scope
- Provider library-count allocations (→ T24); compressed-decode preflights (already done).

## Design notes
- Prefer reusing `kTierBVertexLimit` / `kTierBTriangleLimit` or a scratch constant over a new public
  limit; only invent one with an ADR.
- A limit hit is `ResourceLimit`; keep `MalformedData` for structural errors.

## Done when
- [x] `x64\Release\Tests.ImportIsolation.exe` passes with the new worker case.
- [x] `x64\Release\Tests.ProviderHost.exe` and `x64\Release\Tests.Unit.exe "[provider]"` pass with
      the provider case.
- [x] Hand-off filled in.

## Hand-off

### What landed
- `import-worker/src/UsdAdapter.h`
  - New inline `PrimvarSampleLimit(uint64_t samples)` admits a flattened texcoord primvar only when
    `samples <= kTierBVertexLimit`; a hit is `ResourceLimit`. Reuses the existing Tier B constant
    (no new public/Tier limit, so no ADR) and no allocation.
- `import-worker/src/UsdAdapter.cpp`
  - `ReadTexcoordPrimvar` now returns `ImportErrorCode` (was `bool`). It calls `PrimvarSampleLimit`
    on `values.size()` *before* `attribute.data.resize`, so a small mesh with a hostile indexed or
    Varying primvar cannot size its sample buffer from the flattened count. Structural failures stay
    `MalformedData`; a wrapped `CheckedMultiply` stays `MalformedData`.
  - `RecoverMissingTexcoords` propagates the returned code, so a limit hit reaches the caller as
    `ResourceLimit` instead of being flattened to `MalformedData`.
- `thumbnail-provider/GltfFamilyAdapter.cpp`
  - The transform walk now fails `ErrorCode::ResourceLimit` once `++visited > ProviderLimits::kNodesMax`
    (10000), in addition to the existing `kMaxTransformDepth = 256` depth guard and the deadline
    checkpoint. This mirrors the worker `VisitNode` `kTierAObjectLimit` budget in provider terms and
    stops a mesh-less doubling DAG from re-walking shared subtrees until the deadline. `kNodesMax` is
    already the provider scene-graph node cap, so no new provider/public limit and no ADR.

### Tests
- `tests/import-isolation/UsdSpikeTests.cpp` `[usd-spike][resource-limit]`: new case
  "USD texcoord sample buffers are bounded by the Tier B vertex cap before allocation" — boundary at
  `kTierBVertexLimit` allowed, `+1` and `UINT64_MAX/8` rejected `ResourceLimit`.
- `tests/unit/ProviderGltfAdapterTests.cpp` `[provider][gltf][security]`: new `MeshlessDoublingDagJson`
  fixture (40 levels, each node has two identical children, no mesh) and case asserting
  `Parse() == ResourceLimit` with no emitted triangles.
- `tests/provider-host/ProviderHostTests.cpp` `[host][gltf][security]`: the same DAG driven through
  the real adapter via `CreateFamilyAdapter(Family::Gltf)` and `MemorySource`, asserting
  `ResourceLimit` with no bitmap.

### Check results
- `x64\Release\Tests.ImportIsolation.exe` full: 404 cases, 399 passed, 5 skipped, 0 failed, exit 0.
- `x64\Release\Tests.ImportIsolation.exe "[usd-spike][resource-limit]"`: 12 assertions / 2 cases,
  exit 0. Debug: same, exit 0.
- `x64\Release\Tests.Unit.exe "[provider]"`: 60936 assertions / 222 cases, exit 0.
- `x64\Release\Tests.Unit.exe "[provider][gltf][security]"`: 21 assertions / 5 cases, exit 0.
  Debug: same, exit 0.
- `x64\Release\Tests.Unit.exe` full (harness verify): 135138 assertions / 376 cases, exit 0.
- `x64\Release\Tests.ProviderHost.exe` full: 155 assertions / 9 cases, exit 0.
- `x64\Release\Tests.ProviderHost.exe "[host][gltf][security]"`: 3 assertions / 1 case, exit 0.

### Deviations
- The USD regression is a boundary unit test over the new `PrimvarSampleLimit` helper, not a real
  huge fixture. A USDA whose flattened primvar sample count exceeds 60M needs a multi-hundred-MB
  file; the helper keeps the pre-resize admission check unit-testable, exactly as T01 did for
  `PrimvarExpansionLimit`. The real worker path is still exercised by the existing USD sandbox
  fixtures (full `Tests.ImportIsolation` green).
- Provider cap value: the task allowed `kTierAObjectLimit` (100000) *or* a provider constant. The
  provider already caps `asset.nodes.size()` at `ProviderLimits::kNodesMax` (10000) and instances at
  the same value, so reusing `kNodesMax` keeps one provider source of truth and avoids importing a
  worker/Tier A constant into the independently-built provider. A legitimate mesh-bearing DAG is
  already bounded by the instance cap; a mesh-less DAG now has the same bound.

### Docs changed
- `docs/tasks/security/21-sec01-sweep-completion.md` (this hand-off).
- `docs/security/PROGRESS.md` (T21 reusable facts).
- No ADR: both changes reuse existing constants (`kTierBVertexLimit`, `ProviderLimits::kNodesMax`),
  so no new public/Tier/provider limit was introduced.
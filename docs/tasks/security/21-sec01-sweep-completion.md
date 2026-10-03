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
- [ ] Bound the USD primvar buffer by an explicit limit *before* `attribute.data.resize`; reuse a
      Tier B constant where possible, otherwise add one with an ADR.
- [ ] Add a total-visit cap to the provider glTF walk mirroring `kTierAObjectLimit` (or a provider
      constant), failing `ErrorCode::ResourceLimit`; keep the depth cap and deadline.
- [ ] Regression tests: a USD mesh whose primvar count vastly exceeds its corners is rejected
      without a large allocation; a provider mesh-less doubling DAG fails fast with `ResourceLimit`.
- [ ] Record any new public/Tier limit in an ADR; if an existing constant is reused, note that.

## Out of scope
- Provider library-count allocations (→ T24); compressed-decode preflights (already done).

## Design notes
- Prefer reusing `kTierBVertexLimit` / `kTierBTriangleLimit` or a scratch constant over a new public
  limit; only invent one with an ADR.
- A limit hit is `ResourceLimit`; keep `MalformedData` for structural errors.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe` passes with the new worker case.
- [ ] `x64\Release\Tests.ProviderHost.exe` and `x64\Release\Tests.Unit.exe "[provider]"` pass with
      the provider case.
- [ ] Hand-off filled in.

## Hand-off
_(filled in by the implementing session)_
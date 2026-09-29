# 0016 — Deterministic geometry sampling and the over-cap policy

## Status
accepted

## Context
T04 froze `IGeometrySampler`/`IGeometrySink` (`thumbnail-provider/GeometrySampler.h`,
`FamilyAdapter.h`) and T06 froze the inspect caps (2M triangles / 6M points), the 250k
rasterized-sample cap, the 192 MiB scratch cap, the deadline and the allocation ledger.
design/05 ("Geometry sampling") requires a deterministic spatial/reservoir reduction that
preserves silhouette, material boundaries and disconnected components, with a stable
source-derived seed, and that never renders a biased source prefix when a source exceeds the
inspect cap. The drain was unimplemented: `DefaultThumbnailDependencies::CreateSampler()`
returned `nullptr`. ADR-015 is the viewer's coarse/full sampling decision the provider mirrors
in miniature; the frozen interface is stream-only (`Begin(seed)` + `AddTriangle/AddPoint`), so
it cannot open the source or know its declared bounds itself.

## Decision
- The sampler is `thumbnail-provider/DeterministicGeometrySampler.{h,cpp}` implementing the
  frozen `IGeometrySampler` without widening it. It is PCH/COM/GDI-free and is compiled into
  both the DLL and `Tests.Unit.exe`, like `StreamSource.cpp`/`ThumbnailPipeline.cpp`.
- **Selection is a deterministic min-hash priority reservoir (bottom-k), not sequential
  reservoir sampling.** Each valid sample gets a 64-bit key hashed from the source seed and
  the full sample; the k smallest keys are retained. A sequential reservoir samples by arrival
  position and is not source-order independent, so it would fail the reorder guarantee and
  Explorer's cache consistency. Ties break on the full canonical sample, and the output is
  de-duplicated and sorted by key, so a reordered enumeration yields byte-identical output.
- **Bounds** accumulate from the finite positions of every valid inspected sample.
  Non-finite samples and zero-area/degenerate triangles are discarded.
- **Enumeration continues to the inspect cap** even after the retained reservoir is full.
  Return-false stops the adapter, so stopping at the retained cap would bias the result to a
  source prefix; the retained cap bounds memory, not enumeration.
- **Coverage floor.** Besides the priority reservoir, the sampler keeps one representative per
  occupied **spatial cell** (a bounded 65,536-slot hash grid of 1.0-unit cells in model space)
  and one per **material** (index `0..kMaterialsMax`; out-of-range and `0` share the neutral
  slot). A spatial slot keeps the representative of the smallest cell key that hashes to it, so
  collisions are resolved independently of arrival order. This mirrors ADR-015's
  "one primitive per nonempty region" in miniature; the final set is coverage ∪ the smallest
  priority keys up to `kRasterizedSamplesMax`.
- The single 250k budget is **shared** by triangles and points (`kRasterizedSamplesMax`), per
  design/05's "at most 250,000 representative triangles/points".
- **Accounting.** Retained arrays are reserved against the caller's `AllocationLedger` (the
  production default is `ProcessWide()`) before allocation, in two independent reservations so
  a nearly full ledger can still yield a coverage-only result. The sampler never grows past
  `kMaxRetained`; a failed reservation yields an empty result rather than an unbounded
  allocation.
- **Over-cap policy** is `thumbnail-provider/GeometrySamplingPolicy.h`:
  `DecideGeometrySampling(seekable, declaredPrimitives, inspectCap)` returns `Enumerate` within
  the cap, `StratifiedAcrossExtent` (with `kGeometrySamplingStrata`) over a seek-capable stream,
  and `SafeFallback` for an over-cap stream the provider cannot position. `StratifiedOffsets`
  is the deterministic, evenly spread offset set an adapter reads its strata from. Adapters
  (T21-T34) execute the I/O; the policy lives in one place so every family behaves the same.
- **Rejected:** sequential reservoir sampling (order-dependent); unbounded per-component maps
  (unknown bounds, untrusted counts); retaining the first cap-sized prefix (biased); widening
  the frozen `IGeometrySampler` to take bounds or a source (couples sampling to parsing).

## Consequences
- The provider DLL gains only CRT imports (`api-ms-win-crt-math`, `MSVCP140`) from the sampler;
  the two-symbol `PRIVATE` export surface and the dependency-closure rule are unchanged.
- `Tests.Unit.exe` `[provider][sampler]` covers reorder determinism (including over the cap),
  the inspect caps, spatial/material coverage, finite bounds, NaN/Inf and degenerate rejection,
  the ledger-degradation path, the policy table and `StratifiedOffsets`.
- T15 consumes the result unchanged. T21-T34 must feed **all** inspected samples (to the cap)
  into the sampler and consult `DecideGeometrySampling` before reading an over-cap source; a
  non-seekable over-cap source without a seekable backing falls back to the generic icon. True
  per-source-region coverage beyond the bounded grid still depends on the adapter enumerating
  regions deliberately.
- The 1.0-unit cell size is a fixed local heuristic; it is scale-agnostic only in that it is
  deterministic. Extreme-scale models rely on the priority reservoir for density.
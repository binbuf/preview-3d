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
- [x] Accumulate finite bounds; discard degenerate/non-finite triangles and points.
- [x] Inspect at most 2 M triangles / 6 M points and admit at most 250 k representatives to rasterization; charge retained sample/scratch memory against the T06 aggregate budget ledger.
- [x] Use a deterministic spatial/reservoir sampler with a stable source-derived seed so output is identical across runs and Explorer cache-consistent.
- [x] When the source exceeds the inspect cap, place a deterministic stratified sample across the whole declared extent for seek-capable streams (never a source prefix); a non-seekable over-cap source without trustworthy metadata returns the safe fallback rather than a biased prefix.
- [x] Guarantee minimum representation for material boundaries and large/disconnected components; support a coverage floor of one primitive per nonempty region when caps permit.
- [x] Add tests: reordered input yields the same selection, component coverage, deterministic bytes, hostile counts and NaN/Inf rejection.

## Out of scope
- Per-format geometry extraction (→ T21–T34).
- GPU/LOD simplification (meshoptimizer LOD is a viewer concern, not the provider).

## Design notes
- Point clouds use the same deterministic spatial/reservoir policy as meshes.
- The sampler must never require the whole source in private memory.
- Sampling speed matters against the deadline; keep it allocation-lean and single-pass where possible.

## Done when
- [x] Sampler tests pass in Debug and Release, including reordering determinism and coverage.
- [x] A multi-million-primitive input stays within the inspected/sampled caps and the deadline.
- [x] Hand-off below filled in.

## Hand-off

- **Landed.** `thumbnail-provider/DeterministicGeometrySampler.{h,cpp}` implements the frozen
  `IGeometrySampler` (a deterministic **min-hash bottom-k reservoir**, not sequential reservoir
  sampling, because reorder independence is required); `thumbnail-provider/GeometrySamplingPolicy.h`
  holds the over-cap decision (`DecideGeometrySampling`) and `StratifiedOffsets`. Both compile
  PCH-free into the DLL and `Tests.Unit.exe`; `DefaultThumbnailDependencies::CreateSampler()`
  now returns the real sampler, so `GetThumbnail` reaches the (still unlinked) rasterizer instead
  of failing at `CreateSampler`. ADR-0016 records the decisions; design/05 and interfaces.md note
  the implementation.
- **Caps.** Inspect caps (2 M tri / 6 M pts) stop enumeration; the shared 250 k
  (`ProviderLimits::kRasterizedSamplesMax`) retained budget is charged to the caller's
  `AllocationLedger` (production `ProcessWide()`) before allocation. The reservoir and the
  coverage tables are reserved independently; a failed reservation yields an empty result, which
  `RunThumbnailPipeline` currently maps to `BadFormat` — the frozen `IGeometrySampler` has no
  error channel, so a ledger-induced failure is not distinguishable from empty input there.
- **Coverage.** One representative per occupied spatial cell (65,536-slot grid, 1.0-unit cells,
  slot keeps the smallest cell key so collisions are order-independent) and one per material
  (index `0..kMaterialsMax`, out-of-range → neutral) survive the retained cap; the rest fill by
  priority key. The final set is de-duplicated and sorted by key, so a reordered enumeration is
  byte-identical.
- **Deadline / deviation.** The frozen interface carries no `Deadline`, so the sampler cannot
  poll it; per-sample work is O(1) amortized and the adapter/pipeline own checkpoints. The
  deadline box is met by the adapter enumerating to the cap and polling between bounded units.
  T21–T34 **must** consult `DecideGeometrySampling(seekable, declared, cap)` before reading an
  over-cap source and use `StratifiedOffsets`; a non-seekable over-cap stream is the safe
  fallback. True per-source-region coverage beyond the bounded grid still depends on the adapter
  enumerating regions deliberately.
- **Evidence.** `x64\Release\Tests.Unit.exe` 179 cases / 85041 assertions green (Debug 179 /
  85126); `[provider][sampler]` 11 cases in both. Provider release+debug build clean under
  `/W4 /WX`; exports still exactly the two `PRIVATE` symbols; dependency closure exit 0 (new
  CRT-only imports `api-ms-win-crt-math`, `MSVCP140`, no product/ole32 import). Build with
  `msbuild tests\unit\Tests.Unit.vcxproj /p:Configuration=<Debug|Release> /p:Platform=x64
  /p:SolutionDir=<repo root>\`. Tests deliberately pool 250 k+ samples and run the full 2 M/6 M
  cap loops; Debug is ~15 s for the whole suite.
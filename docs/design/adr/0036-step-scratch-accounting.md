# 0036 — STEP scratch reservation matches worst-case per-definition/cache residency

## Status
accepted

## Context
The STEP adapter charged one flat 96 MiB scratch reservation to the T06 ledger, intended to cover
the parser/tessellation working set it owns (`StepFamilyAdapter.cpp`). The geometry cache and
per-definition build buffers are product-owned `std::vector`s allocated after the reservation is
taken, and their worst case exceeded 96 MiB: a full cache of `kStepMaxGeometryCacheTriangles`
(750,000) triangles could coexist with a temporary per-definition build up to
`kStepMaxTrianglesPerDefinition` (1,000,000) triangles. At 76 bytes per triangle that is ~127 MiB
plus `std::vector` growth slack, so the charge understated the ledger by roughly 30 MiB.

## Decision
Derive the reservation from the caps and the real per-triangle layout instead of a flat number:

- `kStepGeometryBytesPerTriangle` = 9 position floats + 9 normal floats + 1 material index, doubled
  to bound the growth slack of the `push_back`-filled vectors (76 -> 152 bytes/triangle).
- Lower `kStepMaxTrianglesPerDefinition` from 1,000,000 to 500,000.
- Keep `kStepMaxGeometryCacheTriangles` at 750,000 and `kStepMaxTrianglesTotal` at 2,000,000.
- `kStepScratchReservationBytes` = (500,000 + 750,000) * 152 = 190,000,000 bytes (181.2 MiB), with
  a `static_assert` that it stays inside `ProviderLimits::kAccountedScratchMaxBytes` (192 MiB).

The overall emission cap is unchanged, so a scene of several smaller definitions still emits up to
2,000,000 triangles; only a single definition larger than 500,000 triangles now fails closed to
`TessellationFailed` (generic icon) instead of being tessellated.

## Consequences
- The ledger charge is an honest upper bound on the adapter's own geometry residency, so the 384 MiB
  accounting claim is not overcommitted by STEP.
- A single very large definition that previously rendered may now fall back to the generic icon. The
  committed STEP corpus is far below the cap; this is a safety ceiling, consistent with the other
  provider caps.
- OCCT's internal mesher/reader allocations still have no allocator callback and are not charged;
  they remain part of the measured process-commit target T51 records, not the ledger.
- If a future proof allows exact incremental charging of each geometry buffer, this fixed
  reservation can be replaced; the caps and the `static_assert` remain the guard rail.
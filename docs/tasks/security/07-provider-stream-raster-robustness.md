---
verify: x64\Release\Tests.ProviderHost.exe
---

# T07 — Provider stream/raster/accounting robustness

## Goal
The remaining small robustness defects in the provider's stream adapter, rasterizer, and allocation
accounting are fixed or explicitly documented, so the measured 384 MiB surrogate budget is honest
and no path relies on undefined behavior.

## Context (read first)
- `thumbnail-provider/StreamSource.cpp:82-118` — `STATSTG.cbSize` is compared as signed
  `LONGLONG` then stored as `uint64_t`; a hostile `IStream::Stat` returning `-1` yields
  `size_ == UINT64_MAX` and `sizeKnown_ = true`. Downstream paths re-bound it, but clamp explicitly.
- `thumbnail-provider/CpuRasterizer.cpp:307-310,403-406,443-447` — `floor`/`ceil` of model-derived
  doubles convert to `int` without clamping (UB; benign on MSVC x64 because the range check then
  skips the loop). Clamp to the raster extent.
- `thumbnail-provider/CpuRasterizer.cpp:431-467` — the floor-shadow pass has no deadline checkpoint.
- `thumbnail-provider/StepFamilyAdapter.cpp:974-977` — a flat 96 MiB scratch reservation while
  per-definition build/cache can exceed it (`:855-899,1030-1032`); accounting is approximate.
- `thumbnail-provider/AllocationLedger.h:13-24` — library allocations (lib3mf/Draco/OCCT/fastgltf)
  are not charged; `docs/PROGRESS.md` tracks the measurement gap (T51 funding).
- Tests: `tests/unit` provider cases, `tests/provider-host`.

## Scope
- [ ] Reject a negative/oversized `IStream` size explicitly and store an unsigned value only after
      validation; keep the non-seekable materialization cap.
- [ ] Clamp screen-space `double -> int` conversions before the cast in all three rasterizer sites
      (and anywhere the same pattern appears).
- [ ] Add a deadline checkpoint to the floor-shadow pass.
- [ ] Reconcile the STEP scratch reservation with worst-case per-definition/cache accounting, or
      lower the definitions/faces caps so the reservation is true; record the chosen values.
- [ ] Update the provider budget documentation/diagnostics so the 384 MiB claim states exactly what
      is charged and what is library-owned; link the T51 measurement if available.
- [ ] Tests for the negative-size stream, a near-INT_MAX coordinate triangle, and the clamped
      raster extent; assert no crash and bounded output.

## Out of scope
- OCCT singleton concurrency and AV policy (→ SEC-08).
- Full surrogate soak (→ SEC-17).
- Changing the public 384 MiB target without T51 evidence.

## Design notes
- These are small, mechanical hardening changes; keep behavior identical for valid inputs.
- Prefer checked conversions (`CheckedMultiply`/explicit clamp) over casts in any new code.
- If the STEP accounting cannot be made exact, fail closed on the reservation instead of
  overcommitting the ledger.

## Done when
- [ ] `Tests.Unit` and `Tests.ProviderHost` pass in Debug and Release with the new cases.
- [ ] The budget documentation matches what is actually enforced and charged.
- [ ] Hand-off filled in.
## Hand-off

Landed in this session (attempt 1):

- **Stream size validation.** `StreamSource.cpp` now routes every reported size
  (`STATSTG.cbSize` and `Seek(STREAM_SEEK_END)`) through `TryValidateStreamSize`,
  which rejects the high bit (a signed `-1`/unknown sentinel) and anything over
  `kStreamMaxBytes` before storing `size_`/`sizeKnown_`. Both branches now share
  the same validated `statSize`; the non-seekable materialization cap is
  unchanged. Behavior for valid streams is identical.
- **Rasterizer casts + deadline.** `CpuRasterizer.cpp` adds `ClampToRasterExtent`
  (NaN/low -> low, high -> high, else cast) and uses it for all three
  double->int screen-extent sites (triangle clip bounds, point radius bounds,
  floor-shadow bounds/radius). `RenderFloorShadow` now takes `WorkGuard&`,
  returns `bool`, and checkpoints per pixel; `RenderCpuTileRaster` maps an
  expired shadow pass to `Cancelled` (empty image). Golden output unchanged.
- **STEP accounting.** `StepFamilyAdapter.cpp` derives
  `kStepScratchReservationBytes` from the caps and a real per-triangle layout:
  `kStepGeometryBytesPerTriangle` = 152 (9 pos floats + 9 normal floats + 1
  index, doubled for vector growth slack); worst case = (per-definition +
  cache) = 500,000 + 750,000 triangles = 190,000,000 bytes (181.2 MiB). The
  per-definition cap was lowered 1,000,000 -> 500,000 so the single reservation
  is true; a `static_assert` keeps it inside `kAccountedScratchMaxBytes`
  (192 MiB). Overall emission cap (`kStepMaxTrianglesTotal` = 2,000,000) is
  unchanged. Recorded in ADR-0036.
- **Docs.** `docs/design/05-thumbnail-provider.md` and
  `docs/design/03-file-formats-and-ingestion.md` now enumerate exactly what the
  384 MiB ledger charges vs what is library/OS-owned, and note the T51 measured
  target is still open. `docs/design/adapters/step-009-thumbnail.md` records the
  new caps/reservation. ADR-0036 added.
- **Tests.** Negative STATSTG-size and negative seek-end-size stream cases;
  near-INT_MAX triangle and far out-of-frame point raster cases; a STEP case
  proving a 100 MiB ledger is refused by the reconciled reservation while the
  default 384 MiB ledger admits the fixture.

Deviation: the task offered "reconcile the reservation or lower the caps"; both
were done because the existing 1M per-definition cap made an exact reservation
exceed the 192 MiB accounted-scratch cap. Chosen values are recorded in ADR-0036.

Check results (all foreground, this machine):

- `MSBuild tests\unit\Tests.Unit.vcxproj /p:Configuration=Debug|Release` -> OK.
- `x64\Debug\Tests.Unit.exe` -> All tests passed (135067 assertions / 355 cases).
- `x64\Release\Tests.Unit.exe` -> All tests passed (134989 assertions / 355 cases).
- `MSBuild tests\provider-host\Tests.ProviderHost.vcxproj ...` Debug and Release -> OK.
- `x64\Debug\Tests.ProviderHost.exe` and `x64\Release\Tests.ProviderHost.exe` ->
  128 assertions / 6 cases, all pass.

Remaining work / next task must know:

- The public 384 MiB figures were not changed (out of scope; needs T51 evidence).
- T51 still owes the measured process-private-commit peak; STEP's OCCT
  reader/mesher allocations remain library-owned and uncharged by the ledger.
- The floor-shadow checkpoint is not directly unit-tested (deadline timing); the
  code path is covered by the existing expired-deadline raster case only at the
  top-level check.
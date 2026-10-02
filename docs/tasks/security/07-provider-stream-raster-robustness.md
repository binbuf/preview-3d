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
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
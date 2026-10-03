---
verify: x64\Release\Tests.ProviderHost.exe
---

# T24 — Bound provider allocations fed by library-supplied counts (3MF/USD)

## Goal
A hostile 3MF/USD cannot make the provider allocate from an authored count before the product cap
runs, so the 384 MiB ledger is honest and an oversized library result is rejected, not absorbed.

## Context (read first)
- Follow-up audit evidence:
  - `thumbnail-provider/ThreeMfFamilyAdapter.cpp:815-819` `composite->GetComposite(...)` allocates
    then checks `values.size() > 4096`; `:859-862` `GetMultiProperty` similarly; `:1552-1558`
    `GetBeams`/`GetBalls` allocate before the `kLatticeTrianglesMax` check; `:1653-1658`
    `ClipInside` can grow generated triangles past its estimate before the cap is applied.
  - `thumbnail-provider/UsdFamilyAdapter.cpp:1100-1136` loads instancer `protoIndices`/`positions`/
    `ids`/`orientations`/`scales`/`invisibleIds` and builds an `unordered_set` proportional to
    file-controlled lengths with no pre-cap.
- `docs/tasks/security/07-provider-stream-raster-robustness.md` — the accounting model.

## Scope
- [ ] For each library call whose output is file-count-driven, check the authored count against the
      product cap before the call where the library exposes it, or bound the result immediately and
      fail `ErrorCode::ResourceLimit` (not `OutOfMemory`) on overrun.
- [ ] Bound the USD instancer arrays before loading them.
- [ ] Add `ProviderFuzz` seeds and/or `ProviderHost` regressions for the oversized cases.

## Out of scope
- The allocating-`noexcept` holes (→ T20); OCCT/STEP (SEC-17 soak, → T25).

## Design notes
- Where the library exposes no count accessor, the accepted shape is: allocate under the ledger,
  check the size, fail `ResourceLimit`, and keep the peak charged and bounded.
- Do not change the public 384 MiB target without T51 evidence.

## Done when
- [ ] `x64\Release\Tests.ProviderHost.exe` passes with the new cases.
- [ ] Hand-off records the chosen bound and rationale per site.

## Hand-off
_(filled in by the implementing session)_
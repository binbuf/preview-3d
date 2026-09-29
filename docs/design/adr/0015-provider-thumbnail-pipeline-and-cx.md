# 0015 — Provider GetThumbnail pipeline, adapter registry and cx handling

## Status
accepted

## Context
T11/T12 gave the provider a CLSID-routed `ProviderObject` that implements
`IUnknown` and `IInitializeWithStream` over the T12 `BoundedStreamSource`; T04
froze `IFamilyAdapter`/`IGeometrySampler`/`ICpuRasterizer`; T06 froze the
HRESULT table, deadline and ledger. T13 must add `IThumbnailProvider::GetThumbnail`
and wire the CLSID-selected adapter through to a returned `HBITMAP` while the
sampler (T14), rasterizer (T15) and every real family adapter (T21–T34) do not
exist yet. Two contract gaps were unresolved: the HRESULT for the degenerate
`cx == 0` (the design says "rejects only `cx == 0`" but the frozen table lists no
argument row), and how `GetThumbnail` is composed and tested before the pipeline
stages exist.

## Decision
- **`cx == 0` is `E_INVALIDARG`.** A new `ProviderOutcome::BadArgument` row is
  added to `ProviderErrors.h` and mapped to `E_INVALIDARG`; every other `cx` is
  only a hint (`min(cx, 512)` and supersampling stay the rasterizer's job, T15),
  so a larger future request is never a failure. This is the one HRESULT added
  beyond the T06 table, and design/05 now lists the row.
- **The orchestration lives in `thumbnail-provider/ThumbnailPipeline.{h,cpp}`**
  (`RunThumbnailPipeline` + the `IThumbnailDependencies` seam), free of the
  provider PCH, GDI and COM, exactly like `StreamSource.cpp`. It is compiled into
  both the DLL and `Tests.Unit.exe`, so routing and error mapping are proven on
  the real code with deterministic doubles rather than a copy.
- **The linked adapter set is a build-time property**
  (`thumbnail-provider/FamilyAdapterRegistry.{h,cpp}`,
  `CreateFamilyAdapter(Family)`), not a mutable runtime registry: there is no
  process-global mutable cache. Until a family task wires its adapter in, the
  family returns `nullptr` and `GetThumbnail` maps that to the tabulated
  `Unsupported` (`HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)`) generic-icon
  fallback; a fabricated bitmap is never returned.
- **`DefaultThumbnailDependencies()`** is the single production composition
  point: T14 replaces its `CreateSampler()` and T15 its `Render()`; T21–T34 add
  their `CreateFamilyAdapter` case. Its only mutable state is a stateless
  function-local singleton.
- **The DIB/HBITMAP boundary is `thumbnail-provider/RasterBitmap.{h,cpp}`**
  (`CreatePremultipliedDib`): it copies the GDI-free `RasterImage` into a
  top-down 32-bpp `BI_RGB` DIB section and returns the only GDI object created.
  `ComCore.cpp` sets `WTSAT_ARGB` on success; failures leave both out-params
  null/unknown.
- `GetThumbnail` rejects null out-params (`E_POINTER`), an inconsistent call
  order (`E_UNEXPECTED`), and `cx == 0` (`E_INVALIDARG`), then restarts the T12
  source deadline and runs the pipeline. T16 still owns wrapping the call in
  `ActiveCallGuard`.

## Consequences
- The provider's import closure gains `GDI32.dll` (only `CreateDIBSection`); the
  two-symbol `PRIVATE` export surface is unchanged and the dependency-closure
  rule still holds.
- T14/T15 do not change `GetThumbnail`; they implement the seam. T21–T34 each
  add one `FamilyAdapterRegistry.cpp` case. T17's host exercises the same COM
  path.
- `Tests.Unit.exe` covers routing for all eight families, the composed HRESULT
  for each adapter failure class at each stage, empty-geometry/OOM/deadline
  handling, the DIB conversion, and the DLL `GetThumbnail` contract including
  `cx == 0` and the unlinked-adapter fallback.
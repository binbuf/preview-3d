---
verify: x64\Release\Tests.Unit.exe
---
# T13 — Implement family routing and the adapter interface

## Goal
Wire the frozen adapter interface to the COM object so `GetThumbnail` dispatches to the correct
family adapter by CLSID, with no content sniffing, and maps adapter failures to the tabulated
HRESULTs.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — call contract, adapter routing and HRESULT table.
- `docs/tasks/04-freeze-provider-interfaces.md` — the adapter/sampler/raster contracts and routing table.
- `docs/tasks/12-bounded-stream-backing.md` — the bounded input an adapter receives.
- `docs/design/adr/0003-provider-decoder-scope.md` — which optional decoders an adapter may use.

## Scope
- [ ] Implement `IThumbnailProvider::GetThumbnail`: treat `cx` as a maximum physical-pixel hint, reject only `cx == 0`, clamp actual raster resolution independently.
- [ ] Select the adapter from the object's CLSID; never sniff another family or recover a path.
- [ ] Provide the adapter a bounded stream view, the deadline, and the provider limits; receive product-owned geometry/materials or a typed error.
- [ ] Map adapter outcomes to the tabulated HRESULTs, always setting a null bitmap on failure.
- [ ] Add a no-op/test adapter and tests proving routing per CLSID, `cx == 0` rejection, and each error mapping.

## Out of scope
- Real family parsing (→ T21–T34).
- Sampler and rasterizer implementation (→ T14–T15).

## Design notes
- Return a top-down 32-bit premultiplied BGRA DIB section and set `WTS_ALPHATYPE` to `WTSAT_ARGB`.
- Explorer owns the returned `HBITMAP`; release every other GDI object before returning.
- Never fabricate a success bitmap for a failed parse.

## Done when
- [ ] Routing and error-mapping tests pass in Debug and Release.
- [ ] `GetThumbnail` returns the exact HRESULT for each injected adapter failure.
- [ ] Hand-off below filled in.

## Hand-off

**What landed**
- `thumbnail-provider/ThumbnailPipeline.h` / `ThumbnailPipeline.cpp` — the GDI-/COM-/PCH-free
  `RunThumbnailPipeline(ThumbnailRequest, IThumbnailDependencies&, RasterImage&)`: validates the call,
  creates the routed adapter, reserves the bounded material table against the T06 ledger, runs the
  frozen `Initialize`→`Parse`→`EnumerateMaterials`→`EnumerateGeometry` lifecycle, feeds samples to
  the T14 sampler, renders through the T15 `RasterRequest`, always calls `Reset()`, and maps every
  adapter error through `HresultForError`. `DefaultThumbnailDependencies()` is the single production
  composition point. `SourceSeed(family, size)` is the stable per-source seed.
- `thumbnail-provider/FamilyAdapterRegistry.h` / `.cpp` — `CreateFamilyAdapter(Family)`; a build-time
  switch (no mutable runtime registry). Every family returns `nullptr` until T21–T34 add their case,
  which maps to the tabulated `Unsupported` (ERROR_NOT_SUPPORTED).
- `thumbnail-provider/RasterBitmap.h` / `.cpp` — `CreatePremultipliedDib(const RasterImage&, HBITMAP&)`;
  builds a top-down 32-bpp `BI_RGB` DIB section and returns the only GDI object created.
- `thumbnail-provider/ComCore.cpp` — `ProviderObject` now also derives `IThumbnailProvider`:
  `QueryInterface` answers `IID_IThumbnailProvider`, and `GetThumbnail(cx, phbmp, pdwAlpha)` rejects
  null out-params (`E_POINTER`), an uninitialized object (`E_UNEXPECTED`), and `cx == 0`
  (`E_INVALIDARG`), restarts the T12 source deadline, runs the pipeline, and on success converts the
  image and sets `WTSAT_ARGB`. Failures leave `*phbmp == nullptr`, `*pdwAlpha == WTSAT_UNKNOWN`.
- `thumbnail-provider/ProviderErrors.h` — new `ProviderOutcome::BadArgument` → `E_INVALIDARG`
  (the one row beyond the T06 table; ADR-0015, design/05 table updated).
- Tests: `tests/unit/ProviderPipelineTests.cpp` (`[provider][pipeline]`, 8 cases) — routing for all
  eight families, each stage × each mapping class, materials/geometry flow into the rasterizer,
  empty geometry, missing adapter/sampler, failed/empty render, bad pointers/expired deadline/full
  ledger, seed stability. `tests/unit/ProviderRasterBitmapTests.cpp` (`[provider][bitmap]`, 2 cases)
  — DIB layout/bytes and invalid-image rejection. `tests/unit/ProviderThumbnailTests.cpp`
  (`[provider][thumbnail]`, 3 cases) — the DLL COM contract. `tests/unit/ProviderTestSupport.h`
  shares the `MemoryStream`/`ProviderModule` doubles for the new file.
- Build wiring: provider `.vcxproj` adds the three headers + three PCH-free sources and `gdi32.lib`;
  `Tests.Unit.vcxproj` compiles the three sources and the three new test files and links `gdi32.lib`.
  Docs: `docs/design/adr/0015-...md`; `design/05` HRESULT table + COM-core paragraph;
  `design/interfaces.md` ("Routed thumbnail pipeline").

**Deviations / decisions**
- `cx == 0` → `E_INVALIDARG`, added to the frozen table via ADR-0015, because the table listed no
  argument row and `E_POINTER`/`E_UNEXPECTED` are the wrong conditions.
- The sampler/rasterizer are consumed through the frozen interfaces only; T13 does not implement
  them. `DefaultThumbnailDependencies()` returns `nullptr`/`Unsupported` for those stages until
  T14/T15 land, so any real `GetThumbnail` currently fails closed to ERROR_NOT_SUPPORTED — verified
  by the DLL test rather than a fabricated bitmap.
- The `AdapterInput` deadline pointer is `&source->MutableDeadline()`; `GetThumbnail` restarts it at
  entry (the T12 hand-off requirement).
- The material table reserves `kMaterialsMax * sizeof(MaterialPayload)` (≈288 KiB) against the
  process-wide ledger before allocating; a failed reservation is `LimitExceeded`.
- `CreateDIBSection(nullptr, ...)` is used so no HDC or other GDI object is created.

**Check results**
- `msbuild thumbnail-provider\Preview3DThumbnailProvider.vcxproj /p:Configuration=Release|Debug
  /p:Platform=x64 /p:SolutionDir=<root>\` → clean (0 warnings under /W4 /WX).
- `msbuild tests\unit\Tests.Unit.vcxproj /p:Configuration=Release|Debug /p:Platform=x64
  /p:SolutionDir=<root>\` → clean.
- `x64\Release\Tests.Unit.exe` → All tests passed (76723 assertions in 168 test cases);
  `x64\Debug\Tests.Unit.exe` → All tests passed (76808 assertions in 168 test cases).
- `[provider][pipeline],[provider][bitmap],[provider][thumbnail]` → 13 cases / 316 assertions green
  in both configs.
- `dumpbin /exports x64\Release\Preview3DThumbnailProvider.dll` → exactly `DllCanUnloadNow`,
  `DllGetClassObject`.
- `tests\unit\check-provider-dependency-closure.ps1 -Configuration Release|Debug` → exit 0; the DLL
  now imports `GDI32.dll` for `CreateDIBSection` and no product import.

**Next task must know**
- T14 implements `DefaultThumbnailDependencies::CreateSampler()` (replace the `nullptr`) with the
  deterministic sampler; `SamplerGeometrySink`/`MaterialTable` in `ThumbnailPipeline.cpp` already
  forward the adapter's `IGeometrySink`/`IMaterialSink`.
- T15 implements `DefaultThumbnailDependencies::Render()` (replace `UnsupportedRequiredFeature`) with
  the tile rasterizer; it receives `RasterRequest.requestedSize == cx` and must clamp independently.
  The clip/render result is copied to the DIB by `RasterBitmap.cpp`; do not build an HBITMAP elsewhere.
- T21–T34 add one `case Family::X:` in `FamilyAdapterRegistry.cpp` returning the adapter; no content
  sniffing and no path recovery.
- T16 still owns `ActiveCallGuard` around `GetThumbnail` and any SEH/exception boundary; the pipeline
  and adapter calls are currently unguarded.
- `RasterImage` must be non-empty on success or the pipeline reports `DecoderFailure`.
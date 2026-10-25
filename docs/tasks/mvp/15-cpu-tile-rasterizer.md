---
verify: x64\Release\Tests.Unit.exe
---
# T15 — Implement the CPU tile rasterizer and bitmap output

## Goal
Render the sampled geometry to the final Explorer bitmap: a deterministic, framed, lit, depth-tested
tile raster at the correct resolution with a premultiplied BGRA result, no GPU device.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — "CPU renderer".
- `docs/tasks/02-spike-rasterizer-prototype.md` — the validated prototype and its measured budget.
- `docs/tasks/06-budgets-deadlines-hresults.md` — deadline and limit helpers.
- `docs/tasks/14-geometry-sampler.md` — the deterministic sample the rasterizer consumes.

## Scope
- [x] Render at `min(cx, 512)` for nonzero cx, using 2× internal supersampling only when the deadline budget permits; the returned bitmap must fit within `cx` in each dimension.
- [x] Frame verified bounds in a fixed isometric view with ~7% margin; apply model transforms in double precision and clip against the near plane.
- [x] Depth-test tiles; shade with ambient plus two fixed lights using the same neutral palette as the viewer; approximate transparent materials as weighted opaque color.
- [x] Render opaque/masked triangles and depth-tested round splats for point clouds with deterministic size and source/neutral color.
- [x] Downsample in linear space and convert to premultiplied BGRA; produce the `HBITMAP`/DIB section and `WTS_ALPHATYPE`.
- [x] Charge raster targets/scratch against the T06 process-wide product-owned allocation ledger; a charge crossing 384 MiB returns the null-bitmap HRESULT before allocation.
- [x] Add golden-image tests at 32/64/256/512 px plus a transparent/alpha and a point-cloud case.

## Out of scope
- Family-specific material semantics (→ T21–T34 supply product-owned material values).
- GPU/D3D rendering — explicitly not used.

## Design notes
- No text, file path, watermark, network content or nondeterministic animation in the bitmap.
- A failed render returns null with a precise HRESULT; never a partial or fabricated image.
- Release every GDI object except the returned bitmap.

## Done when
- [x] Golden images at all four sizes pass with tolerant perceptual comparison; alpha and point cases pass.
- [x] Measured render time meets the prototype budget on the reference machine.
- [x] Hand-off below filled in.

## Hand-off

### What landed
- `thumbnail-provider/CpuRasterizer.{h,cpp}` — the product-owned tile rasterizer behind the frozen
  `ICpuRasterizer`. `CpuRasterizerImpl.h` names `RenderCpuTileRaster(const RasterRequest&,
  RasterImage&)` and the thin `CpuTileRasterizer`. It consumes the T14 `SampledGeometry` and the
  normalized `model_core::MaterialPayload` table, frames the verified bounds in the fixed
  isometric orthographic view with 7% margin, clips against the near plane, depth-tests, shades
  with ambient + two fixed lights, approximates Blend materials as weighted-opaque, renders
  round depth-tested point splats, resolves supersamples in linear space and returns top-down
  premultiplied BGRA. PCH/COM/GDI-free, compiled into the DLL and `Tests.Unit.exe`.
- `thumbnail-provider/ThumbnailPipeline.cpp` — `DefaultDependencies::Render` now calls
  `RenderCpuTileRaster`; the T13 render stage is live once an adapter exists.
- `tests/unit/ProviderRasterizerTests.cpp` + `thumbnail-provider/goldens/*.pam` — 11
  `[provider][rasterizer]` cases: resolution clamp/reject, determinism + premultiplication,
  golden mesh and point clouds at 32/64/256/512, a golden alpha scene, blend-vs-opaque, masked
  cutoff, depth test, deadline/ledger failure, null/empty geometry, and the `ICpuRasterizer`
  adapter with a null ledger. Hidden `[write-goldens]` regenerates the PAM goldens and hidden
  `[rasterizer-perf]` records the cap render time.
- Wiring: `CpuRasterizer.cpp` (PCH-free) + `CpuRasterizerImpl.h` in the provider vcxproj; the
  same source, the test file and a `PREVIEW3D_PROVIDER_GOLDEN_DIR` macro in `Tests.Unit.vcxproj`.
- Docs: `design/adr/0017-cpu-tile-rasterizer.md` (new), design/05 "CPU renderer" implementation
  note, `design/interfaces.md` rasterizer contract.

### Decisions / deviations
- **Material resolution is defined here, not in the families.** albedo =
  `vertexColor.rgb * baseColorFactor.rgb`; alphaMode Opaque/Mask/Blend; Blend is the
  weighted-opaque approximation; `kMaterialFlagDoubleSided`/`kMaterialFlagUnlit`/`emissiveFactor`
  are honoured. Index 0/out-of-range is `NeutralMaterial()`. Vertex and material colors are
  treated as already linear (as documented in `ProviderTypes.h`/`MaterialPayload.h`); only the
  fixed frost/shadow tints are authored in linear space. T21–T34 still own the actual material
  values.
- **Supersampling gate.** 2x only when `allowSupersample` and the deadline has ≥250 ms remaining,
  else 1x (the prototype's 512 px point cap took ~186 ms at 2x). The default test `Deadline`
  starts at 2 s, so goldens are 2x and deterministic.
- **Ledger.** Targets + output are reserved before allocation; a crossing returns `ResourceLimit`
  (`ERROR_FILE_TOO_LARGE`) with a null image. A null ledger disables accounting for direct unit
  tests (matching the T14 sampler); the pipeline always supplies one.
- **Deadline.** Polled every 2048 work units; expiry returns `Cancelled` (`ERROR_TIMEOUT`).
- No scope change to the design/05 contract, so no superseding ADR beyond the implementation
  record in ADR-0017.

### Check results (Release x64)
```
msbuild thumbnail-provider\Preview3DThumbnailProvider.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir=<root>\   -> clean, 0 warnings (/W4 /WX)
msbuild tests\unit\Tests.Unit.vcxproj /p:Configuration=Release|Debug /p:Platform=x64 /p:SolutionDir=<root>\                       -> clean
x64\Release\Tests.Unit.exe                -> All tests passed (134290 assertions in 190 test cases)
x64\Debug\Tests.Unit.exe                  -> All tests passed (134296 assertions in 190 test cases)
x64\Release\Tests.Unit.exe "[provider][rasterizer]" -> All tests passed (49249 assertions in 11 test cases)
x64\Debug\Tests.Unit.exe   "[provider][rasterizer]" -> All tests passed (49249 assertions in 11 test cases)
x64\Release\Tests.Unit.exe "[write-goldens]"        -> All tests passed (27 assertions in 1 test case)   (regenerates thumbnail-provider/goldens)
x64\Release\Tests.Unit.exe "[rasterizer-perf]"      -> mesh 59.2 ms, points 189.7 ms (512 px, 2x SS; T02 prototype 58.8/185.9 ms)
tests\unit\check-provider-dependency-closure.ps1    -> OK: no viewer/worker/host/core imports (9 modules)
[provider][scaffold] (Release + Debug)    -> green (two-symbol export surface + closure)
```

### Remaining work / blockers
None block T15. The full `Preview3D.slnx` build still cannot complete because
`compatibility-host-step`'s OCCT `x64-windows-static-md` triplet is missing (pre-existing,
unrelated; record for T34/environment follow-up). T16 wraps `GetThumbnail` in `ActiveCallGuard`;
T21–T34 add adapters (a real end-to-end bitmap cannot be produced until one exists).

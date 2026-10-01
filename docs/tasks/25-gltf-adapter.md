---
verify: x64\Release\Tests.Unit.exe
---
# T25 — Implement the glTF/GLB thumbnail adapter

> **E2E slice review point.** Completing this task closes the Tier A breadth slice. Run a fresh end-to-end review now; the pipeline may continue to the next task without waiting.

## Goal
Render stream-contained `.glb` and `.gltf` models in Explorer, including embedded compressed geometry
and images, without ever resolving a sidecar.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — stream ingestion (glTF rules) and limits.
- `docs/design/adr/0003-provider-decoder-scope.md` — Draco/meshopt/KTX2/WebP scope in the provider.
- `docs/design/05-thumbnail-provider.md` and `docs/design/03-file-formats-and-ingestion.md` — the Tier A adapter contract (Tier A has no separate family brief; see `design/README.md`).
- `import-worker/src/GltfAdapter.*`, `import-worker/src/DracoDecodeAdapter.*`, `import-worker/src/TextureTranscodeAdapter.*` — implemented viewer adapters used as policy references.
- `docs/tasks/13-family-routing-adapter-interface.md`, `docs/tasks/14-geometry-sampler.md`, `docs/tasks/17-provider-test-harness.md`.

## Scope
- [x] Route `.glb`/`.gltf` to this adapter via its fixed CLSID; parse GLB or embedded-data-URI glTF only.
- [x] Emit bounded triangles with transforms, normals/UVs/colors as available, and product-owned material values (base color, metallic/roughness, emissive, unlit, alpha mode/cutoff, double-sided).
- [x] Decode bounded Draco and `EXT_meshopt_compression` geometry; decode embedded KTX2/Basis and WebP images; each under provider limits, with optional-image fallback to the default material.
- [x] Treat a missing required geometry buffer/extension or any external `.bin`/image sidecar as the safe generic-icon fallback; never open a path.
- [x] Add fixtures and goldens: embedded GLB mesh, instanced/transformed scene, Draco and meshopt sample, KTX2/WebP textured sample, `.gltf` data-URI sample, and sidecar-dependent/malformed/over-budget cases asserting null-bitmap errors.

## Out of scope
- Local `.bin`/image sidecar resolution (never in the provider; viewer-only).
- Viewer-side WebP/meshopt worker budget changes (owned by TSK-209 in the archived plan).

## Design notes
- No path recovery from `IInitializeWithStream`; a `.gltf` that depends on external files gets no thumbnail.
- Keep a hard per-primitive decode cap so one Draco/meshopt primitive cannot exceed scratch/commit budgets.
- Reuse `model_core` material fallbacks for deterministic neutral shading.

## Done when
- [x] `x64\Release\Tests.Unit.exe` (the `verify:` command) passes for the listed fixtures in Debug and Release.
- [x] A sidecar-dependent glTF returns the generic-icon HRESULT with no filesystem access (verified by the isolation test).
- [x] The family is smoke-checked in Explorer using the T22 procedure.
- [x] Hand-off below filled in.

## Hand-off

**Landed.** `thumbnail-provider/GltfFamilyAdapter.{h,cpp}` (class `preview3d::provider::GltfAdapter`),
registered for `Family::Gltf` in `FamilyAdapterRegistry.cpp` and compiled into the DLL,
`Tests.Unit.exe` and `Tests.ProviderHost.exe`. Parses GLB and embedded-data-URI `.gltf` with fastgltf
`Options::None`; emits node-instanced, double-precision world-transformed triangles with
normals/colors and product-owned material values; decodes bounded Draco and `EXT_meshopt_compression`
geometry and bounded embedded KTX2/Basis and WebP images; any non-`data:` buffer/image URI is
`UnsafeReference` (generic icon, no path opened). Tests: `tests/unit/ProviderGltfAdapterTests.cpp`
(`[provider][gltf]`, 19 cases) plus a `gltf-triangle` provider-host fixture/golden. Smoke scripts and
`ProviderSmokeHost.exe` gained `--gltf`; `fixtures/smoke-cube.glb` committed.

**Deviations / notes.**
- UVs are decoded but dropped: the frozen `VertexSample` has no UV channel and the rasterizer samples
  no texture. Embedded KTX2/WebP images are decoded under the 32 MP aggregate budget and discarded for
  the same reason; this is recorded in ADR-0023 and is the one place this task narrows "emit ... UVs".
- Embedded PNG/JPEG images are structurally validated by fastgltf but not decoded (no thumbnail use);
  ADR-0003 names PNG/JPEG, and design/05 already permits an unavailable optional image to fall back to
  the default material.
- A `.gltf` depending on any external `.bin`/image is rejected outright (`UnsafeReference`) per the
  task design note and design/05, not treated as an optional-image fallback.
- `FASTGLTF_ENABLE_DEPRECATED_EXT=1` must be set in **every** project compiling fastgltf headers
  (added to the provider and provider-host projects; Tests.Unit already had it) to match the vcpkg
  static library's `INTERFACE_COMPILE_DEFINITIONS`. Without it the `fastgltf::Material` layout differs
  and the heap is corrupted (observed as a `Tests.ProviderHost.exe` SIGSEGV).

**Checks (Release x64 unless noted).**
- `x64\Release\Tests.Unit.exe`: 268 cases / 134646 assertions green. `x64\Debug\Tests.Unit.exe`:
  268 / 134727 green.
- `x64\Release\Tests.ProviderHost.exe`: 5 cases / 72 assertions green; Debug likewise.
- `pwsh -File packaging\smoke\Invoke-ProviderSmoke.ps1`: exit 0 (STL+PLY+glTF), each
  reference-vs-Shell `meanAbs=0.0000 maxAbs=0` hosted in `dllhost.exe` with no isolation opt-out.
- `pwsh -File tests\unit\check-provider-dependency-closure.ps1 -Configuration Release`: OK, 14 modules.

**Remaining work.** None blocking. T41 must register the glTF family's second extension (`.gltf`) in
addition to `.glb`; the Tier A breadth E2E review is the next natural checkpoint.
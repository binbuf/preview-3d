# 0023 — glTF adapter: embedded-only fastgltf with bounded compressed decode

## Status
accepted

## Context
T25 must render stream-contained `.glb`/`.gltf` models from the Shell's
`IInitializeWithStream` stream, including embedded compressed geometry and images, without resolving
a sidecar. Design/05 gives the provider no filesystem path and no sidecar capability, and
[ADR-0003](0003-provider-decoder-scope.md) fixes the provider decoder scope to bounded Draco,
meshopt, KTX2/Basis and WebP. The frozen `model_core::MaterialPayload` has no texture slot and the
T15 rasterizer samples no texture, so a decoded embedded image can never change the thumbnail's
material values.

## Decision
1. `GltfAdapter` (files `GltfFamilyAdapter.{h,cpp}`) parses with the provider-local pinned fastgltf
   static library and `fastgltf::Options::None`. Only the GLB BIN chunk and embedded `data:` URIs are
   resolved; a buffer or image carrying a non-`data:` URI is rejected as `UnsafeReference`, so the
   provider never opens a path and a sidecar-dependent `.gltf` gets the generic icon.
2. Node instances are traversed from the default scene (or the implicit roots) and each instance's
   double-precision world transform is applied to emitted triangles; the normal is transformed by the
   inverse-transpose. `COLOR_0` is carried; UVs are dropped (the frozen `VertexSample` has no UV
   channel). Materials carry base color, metallic/roughness, emissive, unlit, alpha mode/cutoff and
   double-sided.
3. Geometry is decoded under the provider limits: uncompressed accessors through a meshopt-aware
   buffer adapter, bounded Draco (96 MiB / 1 million triangles) and bounded
   `EXT_meshopt_compression` decode, with each decoded buffer charged to the T06 ledger before
   allocation and each per-primitive materialization bounded by half of `kAccountedScratchMaxBytes`.
4. Embedded KTX2/Basis and WebP images are decoded under the 32 MP aggregate texture budget and then
   discarded: they are budgeted and validated, but their pixel data cannot cross the frozen material
   contract. A missing, corrupt or over-budget optional image uses the default material and never
   fails valid geometry.
5. `FASTGLTF_ENABLE_DEPRECATED_EXT` must be defined in every project that includes fastgltf headers,
   matching the vcpkg static library's `INTERFACE_COMPILE_DEFINITIONS`. MSBuild consumes vcpkg through
   autolink only and does not apply those definitions, so a mismatch silently changes the
   `fastgltf::Material` layout and corrupts the heap (observed as a crash in `Tests.ProviderHost.exe`
   before the define was added to the provider and host projects).

## Consequences
- The provider DLL's dependency closure adds fastgltf, Draco, meshoptimizer, KTX2/Basis and libwebp;
  all are static in the `x64-windows-static-md` triplet, so `dumpbin /dependents` stays import-clean
  and the T42 SBOM must list them.
- A `.gltf` that depends on external files receives no thumbnail, by design note and design/05; the
  adapter reports `ExternalReferenceDetected()` for the isolation test.
- Because no texture is sampled, the thumbnail is determined by geometry and material factors; the
  compressed-image decode exists to bound and validate that path, not to shade the model. If a future
  task adds texture sampling to the rasterizer, this ADR must be revisited.
- T31–T34 reuse the same embedded-only and per-decoder-budget pattern.
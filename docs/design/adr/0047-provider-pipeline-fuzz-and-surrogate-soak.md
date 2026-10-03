# 0047 — Provider pipeline fuzz target and Explorer-surrogate soak

## Status
accepted

## Context
SEC-17 completes the provider-owned half of `docs/tasks/mvp/43-adversarial-hardening.md`:
the eight family adapters, the bounded stream source, the sampler and the CPU
rasterizer must be fuzzed at their own boundary, and an Explorer-surrogate soak
must show that hostile input cannot crash, hang or leak the host. The existing
worker-format targets (SEC-15/16) exercise the pinned parsers but not the
`BoundedSource` -> `IFamilyAdapter` -> `IGeometrySampler` -> `ICpuRasterizer`
path, and `Tests.ProviderHost.exe` runs in-process rather than in the real
`DllHost` surrogate.

Two constraints shape the design. The pinned third-party parsers (fastgltf,
ufbx, lib3mf, TinyUSDZ, OCCT, draco, ktx) are delivered as prebuilt vcpkg static
libraries built without ASan, so an ASan-instrumented target cannot link them
with the default container annotations. The provider STEP manifest
(`thumbnail-provider/step-occt`) is isolated from the root manifest (ADR-0002),
so a fuzz target that links the STEP adapter needs both `vcpkg_installed` trees
restored.

## Decision
- Add `tests/fuzz/ProviderFuzz.cpp` (+ `.vcxproj`, `prepare_provider_seeds.py`),
  a libFuzzer + ASan target over the real provider sources. One 24-byte envelope
  selects a `Pipeline` (registry -> adapter -> sampler -> rasterizer over a
  memory-backed `BoundedStreamSource`), `Stream`, `Sampler` or `Raster` domain.
  `_DISABLE_STL_ANNOTATION` matches the prebuilt closure exactly as `GltfFuzz`
  does; the target's own translation unit and the product-owned sources stay
  ASan-instrumented. The target links all eight adapters including STEP/OCCT,
  so it needs both vcpkg manifests restored; it is documented in
  `tests/fuzz/README.md` and deliberately left out of the `fuzz-smoke` matrix
  (that job restores only the root manifest) until the isolated `step-occt`
  manifest is restored there.
- Add the surrogate soak as `ProviderSoak.{h,cpp}` compiled into
  `packaging\smoke\ProviderSmokeHost.vcxproj` and reached with `--soak`. It
  drives concurrent STA apartments through `IThumbnailCache::GetThumbnail`
  (`WTS_FORCEEXTRACTION`) so the provider is repeatedly loaded and torn down in
  the real `DllHost.exe` surrogate with thumbnail-cache churn, then asserts no
  failed thumbnail, no persistent surrogate after 30 s, and bounded
  GDI/User/handle/thread/private-byte growth.
- A crash/hang (a `GetThumbnail` failure) is a soak failure, never a pass. A
  stack-overflow/`__fastfail` surrogate death remains an *allowed* failure
  under SEC-08/ADR-0037 (fault code and input recorded, process restarted); a
  contained access violation must surface as a quarantine with later requests
  failing closed. The soak is crash containment, not a security boundary
  (ADR-0005).

## Consequences
- The provider boundary is now fuzzed per family and for the rasterizer, and
  the soak supplies the Explorer-stability/leak evidence the import-worker lane
  cannot. No finding was produced by the committed seeds; the corpus stays
  immutable and the smoke commands are recorded with measured runtime/RSS.
- The STEP adapter's OCCT transfer/tessellation remains un-instrumented (as
  `StepFuzz` already documents); its process containment is still the
  AppContainer/Job suite, and the provider soak re-ran that suite Release-green.
- The `fuzz-smoke` promotion is the remaining follow-up: restore the isolated
  `step-occt` manifest in that job, add `ProviderFuzz` to the matrix, and make
  the fuzz lane a required gate (SEC-17/T13).
# 0.7.0 progress notes

Planning update 2026-10-30: T01–T19 (including T10b) remain completed;
T20–T35 extend the roadmap with rendering-quality work. See
[the follow-up investigation](RENDER-QUALITY-FOLLOWUP.md). No implementation
or completed hand-off is claimed for those new tasks.

Shared notebook for the `0.7.0` task set. Earlier sessions record findings, decisions and
things later tasks must know here; the harness maintains the "Key facts" digest at the top.

The plan, ground rules and per-task verify commands are in
[`../tasks/0.7.0/README.md`](../tasks/0.7.0/README.md); execution order is in
[`ROADMAP.md`](ROADMAP.md).

<!-- symphony:digest:start -->
## Key facts (maintained by symphony — do not edit)

- **T01 — Compatibility corpus and baseline scoreboard**: Manifest: `tests/compat/manifest.json` (schema 1). Entry `path` is relative; Harness: `tests/import-isolation/CompatCorpusTests.cpp`, tag `[compat]`.
- **T02 — Format catalog and content-aware router**: Catalog: `shared/model-core/include/model_core/FormatCatalog.h` (header-only; Reserved append-only `FormatId`s with **no descriptor yet**: `Iges=15`,
- **T03 — Catalog-driven registration, filters and Open As**: Core policy: `shared/model-core/include/model_core/FormatSelection.h`.; CLI helpers: `interactive-viewer/src/app/OpenOptions.h`.
- **T04 - Compatibility severity model and report chunk**: Header: `shared/model-core/include/model_core/Compatibility.h`.; `CompatibilitySeverity { Critical=0, Degraded=1, Ignored=2 }`.
- **T05 - glTF required-extension compatibility**: Policy table: `import-worker/src/GltfAdapter.cpp`, anonymous namespace near; Enabled in the fastgltf mask: Draco, basisu, texture transform, quantization,
- **T06 — Per-format presentation defaults and orientation**: Catalog profile enum: `shared/model-core/include/model_core/FormatCatalog.h`; Pure selection: `model_core::ResolvePresentationProfile(descriptor,
- **T07 — STEP tolerance and import quality**: **Profile version is now 6.** `StepTessellationProfile.h`:; **Request quality bits** in `shared/model-core/include/model_core/
- **T08 — OCCT host: IGES and BREP**: **Wire identities:** `SourceFormatId::Iges = 14`, `Brep = 15` (appended).; **Host route:** STEP keeps opcode 20 / `ParseStepFileRequest` (48 bytes).
- **T09 — OCCT host: XBF and FCStd**: **Wire identities:** `SourceFormatId::Xbf = 16`, `Fcstd = 17` (appended).; **Broker:** `ImportFormat::Xbf`/`Fcstd` are CAD-only, producer
- **T10 — Bounded Assimp compatibility adapter**: **Dependency:** `assimp` 6.0.4#3 from the pinned vcpkg baseline, plus its; **Overlay port:** `packaging/vcpkg-ports/assimp/` copies the baseline port and
- **T10b - 3MF best-effort compatibility**: Required-extension policy lives in; Supported: Core, Materials, Production, Beam, Ball.
- **T11 — USD primitives, points and crease tolerance**: **Fast-path classification.** `import-worker/src/UsdAdapter.cpp` adds; **Preserve fast path.** TinyUSDZ reports an unauthored `subdivisionScheme` as
- **T12 — TGA/DDS textures and ORM ambient occlusion**: **Decoders (product-owned, no new third-party code).**; `import-worker/src/TgaImageDecode.{h,cpp}` — type 2/10 true-color,
- **T13 — HDR/EXR textures and environment assets**: **HDR decoder (product-owned).** `import-worker/src/RadianceHdrDecode.{h,cpp}`:; **EXR decoder.** `import-worker/src/OpenExrDecode.{h,cpp}` compiles the
- **T14 — Studio IBL and neutral tone map (first slice)**: **Environment source.** Product-owned procedural studio, no HDRI/user file.; **Bake shape.** Defaults 128 texels/face, 6 specular mips, 48 GGX samples.
- **T15 — HDR target, exposure, background and presets**: **Pure module.** `interactive-viewer/src/render/Presentation.{h,cpp}` owns the; **HDR target.** `D3D12ViewerPath` creates `R16G16B16A16_FLOAT` `sceneColor`
- **T16 — Ground contact shadow and ambient occlusion**: **Chosen path: analytic soft contact footprint, no extra pass.** ADR-0072; **Pure math.** `interactive-viewer/src/render/GroundShadow.{h,cpp}` owns
- **T17 - Line/strip/fan/point topology**: **Wire version is 11** (`model_core::kCurrentProtocolVersion == 11`).; **LineList contract.** Payload is `[vertex bytes][uint32 indices]`, exactly
- **T18 - Material lobes and transparency**: **Wire version is 12** (`model_core::kCurrentProtocolVersion == 12`). v11 and; Value-initialized `MaterialPayload{}` now supplies the pre-v12 defaults
- **T19 - 0.7.0 compatibility qualification and docs**: **Evidence file:** `docs/0.7.0-QUALIFICATION.md`. Raw reports live in; **Corpus gate:** `x64\Release\Tests.ImportIsolation.exe "[compat]"` — 312
- **T20 - deterministic visual comparison harness and observed baseline**: **Wrapper:** `python tests/render-quality/verify.py --task T20 --configuration Debug --output TestResults/render-quality/T20`.; **Bootstrap/research:** `--capture-only` collects candidates and skips
- **T21 - material and light-contribution diagnostics**: **Wrapper:** `python tests/render-quality/verify.py --task T21 --configuration Debug --output TestResults/render-quality/T21`.; **New profile:** `tests/render-quality/profiles/T21.json`. Generated fixture
- **T21a - Windows Photos-style background and ground highlight**: **Gate:** `python tests/render-quality/verify.py --task T21a --configuration Debug --output TestResults/render-quality/T21a`.; **Profile:** `tests/render-quality/profiles/T21a.json`. Unknown/empty profiles
- **T22 — authored material, UV, normal and AO semantics**: The wire/import/validation/cache contract and the renderer-side shader; **Wire version is 13** (`model_core::kCurrentProtocolVersion == 13`).
- **T23 — texture-budget accounting and complete-load readiness**: The microscope's late failure is reproduced and measured, the false terminal; `state = Failed`, `ImportErrorCode::ResourceLimit` (2),

## Task notes (maintained by symphony — do not edit)

- [T01](progress/T01.md) — Manifest: `tests/compat/manifest.json` (schema 1). Entry `path` is relative
- [T02](progress/T02.md) — Catalog: `shared/model-core/include/model_core/FormatCatalog.h` (header-only
- [T03](progress/T03.md) — Core policy: `shared/model-core/include/model_core/FormatSelection.h`.
- [T04](progress/T04.md) — Header: `shared/model-core/include/model_core/Compatibility.h`.
- [T05](progress/T05.md) — Policy table: `import-worker/src/GltfAdapter.cpp`, anonymous namespace near
- [T06](progress/T06.md) — Catalog profile enum: `shared/model-core/include/model_core/FormatCatalog.h`
- [T07](progress/T07.md) — **Profile version is now 6.** `StepTessellationProfile.h`:
- [T08](progress/T08.md) — **Wire identities:** `SourceFormatId::Iges = 14`, `Brep = 15` (appended).
- [T09](progress/T09.md) — **Wire identities:** `SourceFormatId::Xbf = 16`, `Fcstd = 17` (appended).
- [T10](progress/T10.md) — **Dependency:** `assimp` 6.0.4#3 from the pinned vcpkg baseline, plus its
- [T10b](progress/T10b.md) — Required-extension policy lives in
- [T11](progress/T11.md) — **Fast-path classification.** `import-worker/src/UsdAdapter.cpp` adds
- [T12](progress/T12.md) — **Decoders (product-owned, no new third-party code).**
- [T13](progress/T13.md) — **HDR decoder (product-owned).** `import-worker/src/RadianceHdrDecode.{h,cpp}`:
- [T14](progress/T14.md) — **Environment source.** Product-owned procedural studio, no HDRI/user file.
- [T15](progress/T15.md) — **Pure module.** `interactive-viewer/src/render/Presentation.{h,cpp}` owns the
- [T16](progress/T16.md) — **Chosen path: analytic soft contact footprint, no extra pass.** ADR-0072
- [T17](progress/T17.md) — **Wire version is 11** (`model_core::kCurrentProtocolVersion == 11`).
- [T18](progress/T18.md) — **Wire version is 12** (`model_core::kCurrentProtocolVersion == 12`). v11 and
- [T19](progress/T19.md) — **Evidence file:** `docs/0.7.0-QUALIFICATION.md`. Raw reports live in
- [T20](progress/T20.md) — **Wrapper:** `python tests/render-quality/verify.py --task T20 --configuration Debug --output TestResults/render-quality/T20`.
- [T21](progress/T21.md) — **Wrapper:** `python tests/render-quality/verify.py --task T21 --configuration Debug --output TestResults/render-quality/T21`.
- [T21a](progress/T21a.md) — **Gate:** `python tests/render-quality/verify.py --task T21a --configuration Debug --output TestResults/render-quality/T21a`.
- [T22](progress/T22.md) — The wire/import/validation/cache contract and the renderer-side shader
- [T23](progress/T23.md) — The microscope's late failure is reproduced and measured, the false terminal
<!-- symphony:digest:end -->

# 0.7.0 progress notes

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

## Task notes (maintained by symphony — do not edit)

- [T01](progress/T01.md) — Manifest: `tests/compat/manifest.json` (schema 1). Entry `path` is relative
- [T02](progress/T02.md) — Catalog: `shared/model-core/include/model_core/FormatCatalog.h` (header-only
- [T03](progress/T03.md) — Core policy: `shared/model-core/include/model_core/FormatSelection.h`.
- [T04](progress/T04.md) — Header: `shared/model-core/include/model_core/Compatibility.h`.
- [T05](progress/T05.md) — Policy table: `import-worker/src/GltfAdapter.cpp`, anonymous namespace near
- [T06](progress/T06.md) — Catalog profile enum: `shared/model-core/include/model_core/FormatCatalog.h`
<!-- symphony:digest:end -->

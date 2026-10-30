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

## Task notes (maintained by symphony — do not edit)

- [T01](progress/T01.md) — Manifest: `tests/compat/manifest.json` (schema 1). Entry `path` is relative
- [T02](progress/T02.md) — Catalog: `shared/model-core/include/model_core/FormatCatalog.h` (header-only
<!-- symphony:digest:end -->

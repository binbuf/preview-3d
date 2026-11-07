---
timeoutMin: 240
verify: x64\Release\Tests.ImportIsolation.exe "[valid-file]"
---
# T01 — Valid-file coverage baseline and failure triage

## Goal
Create a manifest-driven `[valid-file]` scoreboard that runs valid files with
supported extensions through the real broker/worker or host path and records
`opened`, `degraded`, or `failed`, the typed error, the geometry kind that was
published, and a closed failure class. This gives every later 0.8.0 task a
measurable target instead of anecdotal “it should open” claims.

## Context (read first)
- `tests/compat/manifest.json` and `tests/import-isolation/CompatCorpusTests.cpp`
  — the existing 0.7.0 compatibility scoreboard and its manifest reader.
- `shared/import-broker/include/import_broker/ImportSession.h` — the real
  import request/result fields, including format, sidecar roots, cancellation,
  budgets and host paths.
- `shared/model-core/include/model_core/ImportError.h` — the closed error
  taxonomy used by `failureClass` and the report.
- `shared/model-core/include/model_core/WireFormat.h` — `TriangleList`,
  `LineList`, `PointList` and the chunk summary fields the scoreboard records.
- `docs/FORMAT-SUPPORT.md` — the exact claimed subset and known limitations.
- `docs/tasks/0.7.0/README.md` — the compatibility-corpus conventions and the
  “measure before fixing” rule this task continues.
- F3D reference: `testing/data/` and the per-reader tests under
  `plugins/*/module/Testing/` show the useful corpus shape (happy path plus
  invalid/edge cases). Do not copy files without checking licenses/provenance.

## Scope
- [ ] Factor the minimal JSON manifest reader/loader out of
  `CompatCorpusTests.cpp` into a shared test helper (for example
  `tests/import-isolation/CompatManifest.{h,cpp}`) without changing the
  existing `[compat]` behaviour.
- [ ] Add `tests/compat/valid-file-manifest.json` (schema 1) with entries:
  `id`, `path`, `format`, `expected` (`opened` | `degraded` | `failed`),
  `expectedError`, `expectedGeometry` (`triangles` | `lines` | `points` |
  `mixed` | `none`), `failureClass` (`none` | `extension` | `primitive` |
  `sniffer` | `importer` | `resource`), optional `expectedWarnings`, optional
  `requiresHost`/`requiresStepHost`, and `tags`.
- [ ] Add `tests/import-isolation/ValidFileCorpusTests.cpp` tagged
  `[valid-file]`. Load the new manifest, call `RunImportSession`, and assert
  the outcome, typed error, and geometry kind. Collect a bounded `INFO`
  summary (id, outcome, error, chunks, triangles/lines/points, elapsed ms).
- [ ] Add a hidden `[write-valid-file-report]` case or a small
  `tests/compat/run-valid-file.ps1` wrapper that writes
  `artifacts/valid-file/<Configuration>.json`. The report contains manifest
  ids only, never source paths or model names.
- [ ] Seed the manifest with small, licensed/generated fixtures covering the
  failure classes:
  - extension aliases: `.stpnc`, `.p21`, `.210` copies of a small STEP file;
  - line-only content: DXF, OFF, DAE, X3D;
  - point-only content: a small point DAE/OFF/X3D fixture;
  - wireframe-only CAD: STEP, IGES, BREP, and an FCStd draft/wireframe part;
  - renamed content: a GLB renamed `.bin`, a DAE renamed `.xml`, a DXF
    renamed `.txt`;
  - a small real-world sample from each family where a licensed fixture is
    already available.
- [ ] Record the baseline in the hand-off: counts by format and by
  `failureClass`, and every `failed` entry with its typed error. Do not “fix”
  failures in this task; later tasks own them.
- [ ] Add the new test file and helper to `Tests.ImportIsolation.vcxproj` and
  update the task README if the verify command changes.

## Out of scope
- Changing any importer or sniffer behaviour. This task only measures.
- The full real-world corpus and coverage report (→ T09).
- New primary formats or large-file streaming (deferred; see the 0.8.0
  roadmap).
- Running the render-quality or app-smoke lanes.

## Design notes
- Reuse `import_broker::RunImportSession`; do not add a second broker harness.
- `expected: failed` is valid for a known gap. The test must assert the
  observed typed error and `failureClass`, not merely “it failed”.
- `expectedGeometry` is derived from validated chunk descriptors, not from a
  worker’s unchecked claim. A file that publishes no geometry but returns
  success is a test failure.
- Keep fixture paths repository-root relative, matching the existing compat
  manifest, so fixtures can live under both `tests/fixtures/` and
  `interactive-viewer/test-assets/`.
- Host-missing entries report `skipped` with a Catch2 `WARN`; they do not fail
  a worker-only developer build.
- The report is evidence only. The checked assertion is `[valid-file]`.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe "[valid-file]"` passes.
- [ ] `x64\Debug\Tests.ImportIsolation.exe "[valid-file]"` passes.
- [ ] A deliberately wrong `expectedError` or `expectedGeometry` fails with
  the fixture id and the observed value, proving the harness is not silently
  passing.
- [ ] The hand-off contains the baseline table and the list of cases each later
  task should flip.
- [ ] No product code outside the test harness changed.

## Hand-off
_(filled in by the implementing session: what landed, the baseline table, deviations, and what the next task must know)_

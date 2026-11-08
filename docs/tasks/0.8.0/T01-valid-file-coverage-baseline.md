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
- [x] Factor the minimal JSON manifest reader/loader out of
  `CompatCorpusTests.cpp` into a shared test helper (for example
  `tests/import-isolation/CompatManifest.{h,cpp}`) without changing the
  existing `[compat]` behaviour.
- [x] Add `tests/compat/valid-file-manifest.json` (schema 1) with entries:
  `id`, `path`, `format`, `expected` (`opened` | `degraded` | `failed`),
  `expectedError`, `expectedGeometry` (`triangles` | `lines` | `points` |
  `mixed` | `none`), `failureClass` (`none` | `extension` | `primitive` |
  `sniffer` | `importer` | `resource`), optional `expectedWarnings`, optional
  `requiresHost`/`requiresStepHost`, and `tags`.
- [x] Add `tests/import-isolation/ValidFileCorpusTests.cpp` tagged
  `[valid-file]`. Load the new manifest, call `RunImportSession`, and assert
  the outcome, typed error, and geometry kind. Collect a bounded `INFO`
  summary (id, outcome, error, chunks, triangles/lines/points, elapsed ms).
- [x] Add a hidden `[write-valid-file-report]` case or a small
  `tests/compat/run-valid-file.ps1` wrapper that writes
  `artifacts/valid-file/<Configuration>.json`. The report contains manifest
  ids only, never source paths or model names.
- [x] Seed the manifest with small, licensed/generated fixtures covering the
  failure classes:
  - extension aliases: `.stpnc`, `.p21`, `.210` copies of a small STEP file;
  - line-only content: DXF, OFF, DAE, X3D;
  - point-only content: a small point DAE/OFF/X3D fixture;
  - wireframe-only CAD: STEP, IGES, BREP, and an FCStd draft/wireframe part;
  - renamed content: a GLB renamed `.bin`, a DAE renamed `.xml`, a DXF
    renamed `.txt`;
  - a small real-world sample from each family where a licensed fixture is
    already available.
- [x] Record the baseline in the hand-off: counts by format and by
  `failureClass`, and every `failed` entry with its typed error. Do not “fix”
  failures in this task; later tasks own them.
- [x] Add the new test file and helper to `Tests.ImportIsolation.vcxproj` and
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
- [x] `x64\Release\Tests.ImportIsolation.exe "[valid-file]"` passes.
- [x] `x64\Debug\Tests.ImportIsolation.exe "[valid-file]"` passes.
- [x] A deliberately wrong `expectedError` or `expectedGeometry` fails with
  the fixture id and the observed value, proving the harness is not silently
  passing.
- [x] The hand-off contains the baseline table and the list of cases each later
  task should flip.
- [x] No product code outside the test harness changed.

## Hand-off

### What landed

- `tests/import-isolation/CompatManifest.{h,cpp}` — the minimal schema-1 JSON
  reader/loader lifted out of `CompatCorpusTests.cpp`, plus the shared closed
  `ImportErrorName` table. `[compat]` behaviour is unchanged (600 assertions
  pass in Debug and Release).
- `tests/compat/valid-file-manifest.json` — schema 1, 20 entries.
- `tests/import-isolation/ValidFileCorpusTests.cpp` — `[valid-file]` checked
  cases, `[valid-file-self-check]` wrong-expectation probe, and the hidden
  `[write-valid-file-report]` evidence case.
- `tests/fixtures/valid-file/` — generated fixtures, licensed real-world
  samples, `generate_fixtures.py`, and `README.md` provenance.
- `Tests.ImportIsolation.vcxproj` — new sources and
  `PREVIEW3D_VALID_FILE_MANIFEST`.
- `docs/design/adr/0095-valid-file-scoreboard-open-gate.md` — gate and
  failure-class semantics.
- `tests/compat/README.md` — valid-file scoreboard section.

### Baseline (identical in Debug and Release)

20 entries: **opened 3 · degraded 1 · failed 16 · skipped 0**.

By format: `3mf 1, brep 1, dae 3, dxf 3, fbx 1, fcstd 1, gltf 1, iges 1,
off 2, step 4, x3d 2`.

By failure class: `extension 4, importer 1, none 4, primitive 8, resource 1,
sniffer 2`.

| id | outcome | typed error | class |
| --- | --- | --- | --- |
| `step-alias-stpnc` | failed | `UnsupportedFormat` | extension |
| `step-alias-p21` | failed | `UnsupportedFormat` | extension |
| `step-alias-210` | failed | `UnsupportedFormat` | extension |
| `line-dxf-polyline` | failed | `EmptyGeometry` | primitive |
| `line-off-edges` | failed | `MalformedData` | importer |
| `line-dae-lines` | failed | `EmptyGeometry` | primitive |
| `line-x3d-indexedlineset` | failed | `EmptyGeometry` | primitive |
| `points-dae-points` | opened (8 triangles) | none | none |
| `points-x3d-pointset` | failed | `EmptyGeometry` | primitive |
| `wire-step` | failed | `EmptyGeometry` | primitive |
| `wire-iges` | failed | `EmptyGeometry` | primitive |
| `wire-brep` | failed | `EmptyGeometry` | primitive |
| `wire-fcstd` | failed | `EmptyGeometry` | primitive |
| `renamed-glb-bin` | failed | `UnsupportedFormat` | extension |
| `renamed-dae-xml` | failed | `UnsupportedFormat` | sniffer |
| `renamed-dxf-txt` | failed | `UnsupportedFormat` | sniffer |
| `real-teapot-off` | opened (880 triangles) | none | none |
| `real-pinkegg-dxf` | opened (576 triangles) | none | none |
| `real-animatedworld-fbx` | degraded (`texture-fallback`, 512 triangles) | none | none |
| `real-cubegears-3mf` | failed | `ArchiveLimit` | resource |

The reports are `artifacts/valid-file/Debug.json` and
`artifacts/valid-file/Release.json` (ids and stable format names only).

### Cases later tasks should flip

- **T02** — `step-alias-stpnc`, `step-alias-p21`, `step-alias-210`:
  `failed`/`UnsupportedFormat`/`extension` → `opened` through the STEP host.
- **T03** — `renamed-dae-xml`, `renamed-dxf-txt`: `sniffer` → `extension`
  once the catalog-driven probe identifies DAE/DXF content. The outcome stays
  `failed` at the automatic gate because the product correctly asks the user
  through Open As; T03's unit cases own the prompt proof.
- **T04** — `line-dxf-polyline`, `line-dae-lines`, `line-x3d-indexedlineset`,
  `points-x3d-pointset`: `primitive` → `opened` with `lines`/`points`.
  `points-dae-points` is currently `opened` with 8 **triangles**; flip the
  geometry kind to `points`. `line-off-edges` is `MalformedData` because
  Assimp's OFF loader plus `aiProcess_Triangulate`/validation rejects
  two-index edge faces; T04 must adjust the Assimp flags or drop OFF from the
  line-only claim.
- **T05** — `wire-step`, `wire-iges`, `wire-brep`, `wire-fcstd`: `primitive`
  → `opened` with `lines`.
- **T09** — `real-cubegears-3mf`: `ArchiveLimit` is a real resource gap; give
  it an opened outcome or a documented disposition. T09 also adds the
  remaining real-world families (current samples cover OFF, DXF, FBX and 3MF).

### Deviations / decisions

- The scoreboard measures the shipping viewer's **automatic open gate**,
  not an emulated Open As choice. Content-only renamed files therefore stay
  `failed` with a typed `UnsupportedFormat`; their class flips when the
  sniffer learns the format. Recorded in ADR-0095.
- `line.off` uses the OFF optional edge list and reaches Assimp, but the
  current pipeline rejects two-index edge faces as `MalformedData`. It is a
  measured importer gap owned by T04, not a fixture defect.
- `points.dae` currently opens as triangles (8), which is a fidelity gap T04
  should flip to `points`.
- `real-cubegears-3mf` is a licensed real-world sample that hits
  `ArchiveLimit`; it is recorded as a resource-class gap for T09.

### Checks run

```powershell
x64\Release\Tests.ImportIsolation.exe "[valid-file]"          # 163 assertions, 2 cases, pass
x64\Debug\Tests.ImportIsolation.exe   "[valid-file]"          # 163 assertions, 2 cases, pass
x64\Release\Tests.ImportIsolation.exe "[valid-file-self-check]" # 5 assertions, pass
x64\Debug\Tests.ImportIsolation.exe   "[valid-file-self-check]" # 5 assertions, pass
x64\Release\Tests.ImportIsolation.exe "[write-valid-file-report]" # pass
x64\Debug\Tests.ImportIsolation.exe   "[write-valid-file-report]" # pass
x64\Release\Tests.ImportIsolation.exe "[compat]"              # 600 assertions, pass
x64\Debug\Tests.ImportIsolation.exe   "[compat]"              # 600 assertions, pass
```

Wrong-expectation proof: temporarily changing every `expectedGeometry` from
`triangles` to `lines` made `[valid-file]` fail with, for example,
`fixture 'real-teapot-off': expected geometry lines but observed triangles`.
The manifest was restored and the checked run passes again.

### No product code changed

`git status` contains only `tests/`, `docs/`, and the task set files. No
importer, broker, host, wire-format or viewer source changed.

### What the next task must know

Use the shared `compat_test` helper; do not add a second manifest reader. When
an owning task changes behaviour, update its manifest row in the same change
so `[valid-file]` stays the gate. See `docs/0.8.0/progress/T01.md` for
commands, gotchas and follow-ups.



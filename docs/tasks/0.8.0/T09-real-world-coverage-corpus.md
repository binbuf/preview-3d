---
timeoutMin: 300
verify: x64\Release\Tests.ImportIsolation.exe "[format-coverage]"
---
# T09 — Real-world per-format corpus and coverage report

## Goal
Prove coverage with real-world files, not only generated happy-path fixtures.
Every importable catalog extension must have at least two valid files from
different exporters plus one edge/negative file, and a CI report must show
`opened`/`degraded`/`failed` per format. This is the evidence layer that keeps
the 0.8.0 claims honest.

## Context (read first)
- `tests/compat/manifest.json`, `tests/compat/valid-file-manifest.json` —
  the existing compatibility and valid-file scoreboards from T01.
- `tests/fixtures/` — generated and checked-in fixture conventions.
- `docs/FORMAT-SUPPORT.md` — the claimed extensions and subsets.
- `docs/tasks/0.7.0/T01-compatibility-corpus.md` — fixture/license policy and
  the “manifest ids only in reports” rule.
- `docs/tasks/0.8.0/T01-valid-file-coverage-baseline.md` — the failure-class
  schema this task extends.
- F3D reference: `testing/data/` has per-format happy-path and invalid/edge
  files (for example `draft_wireframe.FCStd`, `infinite_line.brep`,
  `broken_links.FCStd`, and `invalid_*` variants). Use it as a shape reference
  only; check every license and record provenance.

## Scope
- [ ] Define a coverage matrix from `model_core::ImportableFormatDescriptors()`:
  every extension must have at least two valid entries from different
  exporters and at least one edge/negative entry.
- [ ] Add small, redistributable real-world fixtures for the families that
  currently have none or only generated fixtures:
  IGES, BREP, XBF, FCStd, DAE, 3DS, DXF, DirectX, OFF, AMF, X3D, and the
  STEP aliases. Prefer small files; keep large qualification assets local and
  out of the repository.
- [ ] If a fixture cannot be committed, add a generated equivalent that
  exercises the same parser path and record why the real-world file is not
  redistributable. Do not silently omit a format.
- [ ] Add `format:<stable-name>` and `real-world` tags to the manifest entries
  so the report can group by format without parsing paths.
- [ ] Add `[format-coverage]` (or extend `[valid-file]`) to assert:
  - every importable extension has at least two valid entries and one edge/
    negative entry;
  - each entry has a typed expected outcome and geometry kind;
  - no entry is unaccounted (no `Unknown` format, no missing file);
  - the report contains no source paths or model names.
- [ ] Add a report writer (hidden Catch2 case or `tests/compat/report-coverage.ps1`)
  that writes `artifacts/coverage/<Configuration>.json` with per-format counts:
  `valid`, `opened`, `degraded`, `failed`, `skipped`, and failure classes.
- [ ] Run the report on Release and Debug and record the matrix in the
  hand-off. A format with a `failed` valid entry must name the owning task or
  a follow-up; do not mark it “covered” without a disposition.
- [ ] Update `tests/compat/README.md` with the coverage policy and the
  distinction between generated, real-world, and restricted-local fixtures.

## Out of scope
- Fixing importer failures. This task measures and reports; the owning tasks
  fix. A failure can be recorded with a typed disposition.
- Adding new primary formats. The matrix covers the current catalog only.
- Committing large downloaded assets or restricted corpus files.
- Render-quality images; this is an import/open coverage lane.

## Design notes
- Two files from “different exporters” means different producer families, not
  two copies of the same file renamed. Record the producer in the fixture
  metadata or a `provenance` field.
- Keep fixtures small enough for CI. If a real file is large, generate a
  reduced equivalent and keep the large original in the local restricted
  corpus.
- The report must be reproducible and path-free. Use manifest ids and stable
  format names only.
- A `degraded` entry is success with a compatibility record; it is not a
  failure. A `failed` entry with a known typed error is a measured gap, not a
  silent pass.
- Do not add a second manifest parser; reuse the T01 helper.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe "[format-coverage]"` passes.
- [ ] `x64\Debug\Tests.ImportIsolation.exe "[format-coverage]"` passes.
- [ ] Every importable extension has at least two valid entries and one
  edge/negative entry.
- [ ] The coverage report exists for both configurations and is path-free.
- [ ] The hand-off contains the per-format matrix and every failure’s owner.
- [ ] Fixture licenses/provenance are recorded; no restricted asset was
  committed.

## Hand-off
_(filled in by the implementing session: matrix, fixture provenance, report paths, and the remaining failures with owners)_

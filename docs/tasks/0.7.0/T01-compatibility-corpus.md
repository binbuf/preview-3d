---
timeoutMin: 240
verify: x64\Release\Tests.ImportIsolation.exe "[compat]"
---
# T01 — Compatibility corpus and baseline scoreboard

## Goal
A manifest-driven `[compat]` suite runs a representative file corpus through the
real broker/worker path and records `opened`, `degraded`, `failed`, the typed
error and the warning list. This gives every later 0.7.0 task a measurable
open-rate scoreboard instead of anecdotal "it opens now" claims.

## Context (read first)
- `docs/proposal.md` §2 and Phase 0 — the failure classes this corpus must
  cover.
- `tests/import-isolation/ImportSessionTests.cpp` — existing
  `import_broker::RunImportSession` request/result patterns and fixture helpers.
- `shared/import-broker/include/import_broker/ImportSession.h` — the request and
  result fields, including `sourcePath`, `format`, section capacity, sidecar
  roots and cancellation.
- `interactive-viewer/test-assets/corpus/manifest.json` — the byte-exact
  fixture manifest pattern already used by the app smoke.
- `docs/tasks/security/01-gltf-traversal-and-limit-ordering.md` — the
  pre-existing full-suite failure warning; do not attribute old failures to
  this task.

## Scope
- [ ] Add `tests/compat/README.md` describing the outcome schema and fixture
  policy. License-check any external fixture before committing it; if a fixture
  comes from F3D's `testing/data`, record its origin and license or regenerate an
  equivalent fixture.
- [ ] Add `tests/compat/manifest.json` (schema 1) with entries:
  `id`, `path`, `format`, `expected` (`opened` | `degraded` | `failed`),
  `expectedError`, `expectedWarnings` (optional), `tags`, and
  `requiresHost`/`requiresStepHost` flags. Seed it with the existing Preview3D
  corpus only; later tasks append their own entries.
- [ ] Add `tests/import-isolation/CompatCorpusTests.cpp` tagged `[compat]`.
  Load the manifest relative to the test asset root, call
  `RunImportSession` for each entry, and assert the outcome/error. Collect a
  bounded summary with `INFO` (id, outcome, error, chunks, elapsed ms) so a
  failure names the fixture.
- [ ] Add a hidden `[write-compat-report]` case or a small
  `tests/compat/run-compat.ps1` wrapper that writes
  `artifacts/compat/<Configuration>.json`. The report is evidence only; the
  checked assertion is the `[compat]` test.
- [ ] Make missing host executables a typed skip, not a crash: entries with
  `requiresHost`/`requiresStepHost` should report `skipped` unless the host
  path resolves, so the corpus is runnable on a worker-only developer build.
- [ ] Record the baseline in the hand-off: per-family counts and every failure
  reason. Do not "fix" failures in this task; the later tasks own them.
- [ ] Add the new test file to `Tests.ImportIsolation.vcxproj` and update
  `docs/tasks/0.7.0/README.md` if the verify command changes.

## Out of scope
- Generating all future fixtures (each breadth task owns its own).
- Changing any importer behaviour; this task only measures.
- Running the full app-smoke/visual corpus (→ T19).

## Design notes
- Reuse `import_broker::RunImportSession`; do not add a second broker harness.
- Keep the manifest path handling platform-neutral (wide paths on Windows).
- Do not commit large downloaded assets. Small, licensed fixtures only; large
  qualification files stay in the performance lane.
- A `degraded` outcome must be distinguishable from `opened`: it means geometry
  published and at least one compatibility warning was recorded, not merely that
  the importer returned success.
- The report must never contain a full source path or model names in a form
  that would be unsafe to attach to an issue; use the manifest `id`.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe "[compat]"` passes.
- [ ] `x64\Debug\Tests.ImportIsolation.exe "[compat]"` passes.
- [ ] A deliberately broken manifest entry fails with the fixture id and the
  observed typed error, proving the harness is not silently passing.
- [ ] The hand-off contains the baseline table and the list of fixtures each
  later task should add.
- [ ] No product code outside the test harness changed.

## Hand-off
_(filled in by the implementing session: manifest path, baseline counts, known
skips, and the exact fixture slots reserved for T05/T07/T08/T09/T10/T11/T12/T13.)_

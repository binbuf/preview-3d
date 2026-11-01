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

### Landed

- `tests/compat/manifest.json` — schema 1, seeded with 36 entries from the
  existing Preview3D corpus (26 glTF, 3 STL, 6 PLY, 1 STEP). Entry `path` is
  repository-root relative because fixtures span `interactive-viewer/test-assets/`
  and `tests/fixtures/`.
- `tests/compat/README.md` — outcome schema, warning ids, fixture/license
  policy, reporting rules and reserved breadth-task slots.
- `tests/import-isolation/CompatCorpusTests.cpp` — `[compat]` corpus runner plus
  hidden `[write-compat-report]`; minimal in-test JSON reader, typed
  `opened`/`degraded`/`failed`, host-missing skip, path-free report at
  `artifacts/compat/<Configuration>.json`.
- `Tests.ImportIsolation.vcxproj` — new source file and
  `PREVIEW3D_COMPAT_MANIFEST`, `PREVIEW3D_REPO_ROOT`,
  `PREVIEW3D_BUILD_CONFIGURATION` defines.
- `docs/design/testing-strategy.md` — compatibility corpus section.
- `docs/design/adr/0056-compatibility-corpus-scoreboard.md` — outcome and
  ownership decision for later tasks.
- No product code outside the test harness changed.

### Baseline (Release, current tree)

| Format | Opened | Degraded | Failed |
| --- | ---: | ---: | ---: |
| glTF | 13 | 1 | 12 |
| STL | 1 | 0 | 2 |
| PLY | 4 | 0 | 2 |
| STEP | 1 | 0 | 0 |
| **Total** | **19** | **1** | **16** |

15 of the 16 failures are intentional negative fixtures:
`sidecar-missing` = `FileUnavailable`; the eight traversal/absolute/network/UNC/
ADS/NUL sidecar fixtures = `UnsafeReference`; `sparse-invalid`,
`truncated-glb`, `truncated-stl` = `MalformedData`; `over-limit-stl`,
`over-limit-ply` = `ResourceLimit`; `empty-ply` = `EmptyGeometry`.
`basisu_corrupt_ktx2` is the one `degraded` row (`texture-fallback`).

The one unintended baseline failure is **`Draw-heavy`**:
`ResourceLimit` at `WorkerReportedError`, zero chunks. It is recorded as
`expected: failed` with tag `known-failure`; this task measures only, so the
owning instancing/catalog work (T02/T17) must classify or fix it.

### Known skips

No skips on this machine: both `Preview3DImportHost.exe` and
`Preview3DStepHost.exe` exist in `x64\Debug` and `x64\Release`. On a worker-only
build, entries with `requiresHost`/`requiresStepHost` report `skipped` in the
JSON report and emit a Catch2 `WARN`; the checked assertion does not fail.

### Commands and results

- `msbuild Preview3D.slnx /t:Tests_ImportIsolation /p:Configuration=Debug /p:Platform=x64`
  — success.
- `msbuild Preview3D.slnx /t:Tests_ImportIsolation /p:Configuration=Release /p:Platform=x64`
  — success.
- `x64\Debug\Tests.ImportIsolation.exe "[compat]"` — **All tests passed
  (312 assertions in 2 test cases)**.
- `x64\Release\Tests.ImportIsolation.exe "[compat]"` — **All tests passed
  (312 assertions in 2 test cases)**.
- `x64\Release\Tests.ImportIsolation.exe "[write-compat-report]"` — passed,
  wrote `artifacts/compat/Release.json` (36 entries).
- Deliberately broken entry: temporarily changed `truncated-glb`'s
  `expectedError` from `MalformedData` to `ResourceLimit`; `[compat]` failed
  with `id=truncated-glb outcome=failed error=MalformedData
  stage=WorkerReportedError`, the observed typed error and the fixture id.
  Manifest restored and re-run green.

### Reserved fixture slots

T05 glTF required/optional extensions; T07 STEP tolerance/quality; T08
IGES/BREP (`requiresStepHost`); T09 XBF/FCStd (`requiresStepHost`); T10 Assimp
long tail; T10b 3MF best effort; T11 USD primitives/points/creases
(`requiresHost`); T12 TGA/DDS/ORM; T13 HDR/EXR. Details are in
`tests/compat/README.md`.

### Deviations

Entry paths are repository-root relative rather than test-asset relative, so
the same manifest can name both `interactive-viewer/test-assets/` and
`tests/fixtures/`; the manifest itself is located by a compiled macro.

### Remaining work

`Draw-heavy`'s pre-existing `ResourceLimit` is intentionally left for the
owning breadth task; all other checks are green.

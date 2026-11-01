# Compatibility corpus

`tests/compat/manifest.json` is the shared, manifest-driven scoreboard for
0.7.0 format compatibility. Every entry runs through the product's real broker
path (`import_broker::RunImportSession`), never a parser-only shortcut, so an
entry measures what a user would actually get.

The checked assertion is:

```powershell
x64\Release\Tests.ImportIsolation.exe "[compat]"
```

The hidden evidence case is:

```powershell
x64\Debug\Tests.ImportIsolation.exe "[write-compat-report]"
```

It writes `artifacts/compat/<Configuration>.json`. That report is evidence
only; the `[compat]` assertion remains the gate.

## Outcome schema

`schema` is `1`. Each entry has:

| Field | Required | Meaning |
| --- | --- | --- |
| `id` | yes | Stable, unique, path-free fixture name used in failures and reports |
| `path` | yes | Fixture path relative to the repository root |
| `format` | yes | `gltf`, `stl`, `ply`, `obj`, `fbx`, `usd`, `3mf` or `step` |
| `expected` | yes | `opened`, `degraded` or `failed` |
| `expectedError` | yes | `none` for opened/degraded; otherwise an `ImportErrorCode` name |
| `expectedWarnings` | no | Exact warning identifiers, e.g. `["texture-fallback"]` |
| `tags` | no | Free-form lane/family labels for filtering in later tasks |
| `requiresHost` | no | Requires `Preview3DImportHost.exe`; typed skip when absent |
| `requiresStepHost` | no | Requires `Preview3DStepHost.exe`; typed skip when absent |

Outcomes are defined from the broker result, not from a parser's own
self-report:

- **opened** — `result.ok`, at least one geometry chunk, and no compatibility
  warning.
- **degraded** — `result.ok`, at least one geometry chunk, and at least one
  compatibility warning. A success with no warning is `opened`, never
  `degraded`.
- **failed** — `result.ok` is false. `expectedError` must equal the typed
  `model_core::ImportErrorCode` name; `none` is not a valid failure.
- **skipped** — only in the evidence report, and only because a required host
  executable is absent. The `[compat]` case warns and continues; it does not
  fail a worker-only developer build.

Current warning identifiers are derived from closed wire facts:

- `optional-feature` — `ImportStatusPayload::optionalFeatureWarnings > 0`.
- `texture-fallback` — `ImportStatusPayload::textureWarnings > 0` or a
  `ChunkTopology::TextureWarning` chunk.

The report contains manifest ids, outcome, typed error, warning identifiers,
chunk/batch counts and elapsed milliseconds. It never contains a source path or
a model name, so it is safe to attach to an issue.

## Fixture policy

- Seed entries are fixtures that already existed in this repository when 0.7.0
  started. Later breadth tasks append their own entries; they do not replace
  this baseline.
- Prefer small fixtures. Large qualification files stay in the performance lane.
- Every externally sourced fixture must record its origin and license next to
  the fixture (or in this directory) before commit. Do not add a fixture whose
  license is unknown. The existing `tests/fixtures/*/` README and license files
  remain authoritative for those fixtures.
- A fixture may be binary; do not regenerate it casually. `id` is the stable
  handle, so a file can be replaced without renaming the scoreboard row.
- Negative fixtures are first-class: malformed, over-limit and unsafe-sidecar
  cases prove the typed failure stays typed.

## Adding an entry

1. Put the fixture in an existing corpus/fixture tree (or reuse one).
2. Add a manifest entry with the expected outcome and, for failures, the typed
   error. Use `expectedWarnings` only when the warning set is part of the
   contract.
3. Run `x64\Debug\Tests.ImportIsolation.exe "[write-compat-report]"` and inspect
   `artifacts/compat/Debug.json` before updating the checked expectation.
4. Run both `[compat]` configurations. Do not "fix" a failure in the manifest;
   a changed outcome is an importer change owned by a breadth task.

## Reserved fixture slots

| Task | Family / slot |
| --- | --- |
| T05 | glTF required-extension entries: required vs optional extension, extension mask |
| T07 | STEP tolerance/quality entries: healing, unit mismatch, tessellation budget |
| T08 | IGES and BREP entries (`requiresStepHost`) |
| T09 | XBF and FCStd entries (`requiresStepHost`) |
| T10 | Assimp long-tail entries (DAE, 3DS, OFF, AMF) |
| T10b | 3MF best-effort compatibility entries |
| T11 | USD primitives, points and crease-tolerance entries (`requiresHost`) |
| T12 | TGA/DDS texture and ORM entries |
| T13 | HDR/EXR texture and environment entries |

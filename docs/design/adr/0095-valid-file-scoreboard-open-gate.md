# 0095 - The valid-file scoreboard measures the automatic open gate

Status: accepted (2026-11-07).

## Context

0.8.0 starts with a valid-file scoreboard (T01) so later tasks can measure
"this valid file opens" instead of asserting it. The 0.7.0 compatibility
corpus (`tests/compat/manifest.json`) is extension-declared: every entry names
a format and runs the broker even when the shipping viewer would not have
reached it. That is the right shape for compatibility evidence, but it cannot
measure the two most common valid-file gaps: a valid suffix the catalog does
not know, and content that the bounded sniffer cannot identify.

The shipping viewer's automatic open gate is explicit:
`ClassifyForOpen` combines `FindByExtension` with `SniffFile`, and
`DecideOpenFormat` opens only `OpenExtension`. A content-only or cross-family
mismatch returns `AskUser` (the Open As failure card) and a file with no
evidence returns `Reject`; neither silently starts an importer. A forced
format from Open As is a separate, user-driven path.

## Decision

`tests/compat/valid-file-manifest.json` (schema 1) records, per entry: `id`,
repository-relative `path`, catalog `format`, `expected`
(`opened`/`degraded`/`failed`), `expectedError`, `expectedGeometry`
(`triangles`/`lines`/`points`/`mixed`/`none`), `failureClass`
(`none`/`extension`/`primitive`/`sniffer`/`importer`/`resource`), optional
`expectedWarnings`, optional `requiresHost`/`requiresStepHost`, and `tags`.

The `[valid-file]` case resolves every entry through the same
`ClassifyFormat`/`DecideOpenFormat` gate the viewer uses:

- `OpenExtension` reaches `import_broker::RunImportSession` with the
  extension's catalog descriptor. No second broker harness exists.
- `AskUser` and `Reject` fail typed with `UnsupportedFormat` before any
  parser runs. An unknown suffix with a content candidate is class
  `extension` (the suffix gap); an unknown suffix with no candidate is class
  `sniffer`. A known suffix that disagrees with strong content is also class
  `extension` because the suffix is the actionable mismatch.
- Import failures are class `primitive` for `EmptyGeometry`, `resource` for
  the closed resource-limit family, and `importer` for every other typed
  error. Success without any validated geometry chunk is a harness failure,
  never `opened`.
- `expectedGeometry` is derived from the validated chunk descriptors
  (`TriangleList`/`LineList`/`PointList`), not from a worker claim.
- A missing required host executable is reported as `skipped` with a Catch2
  `WARN`; it does not fail a worker-only developer build.
- The hidden `[write-valid-file-report]` case writes
  `artifacts/valid-file/<Configuration>.json` with manifest ids and stable
  format names only. The checked assertion remains `[valid-file]`.

## Consequences

- T02 flips the `.stpnc`/`.p21`/`.210` entries from `failed`/`extension` to
  the STEP host's real outcome once the catalog owns the aliases.
- T03 flips the renamed DAE/DXF entries from `sniffer` to `extension` when the
  catalog-driven probe identifies their content. Their outcome stays `failed`
  at the automatic gate because the product correctly asks the user through
  Open As; T03's unit cases prove the prompt, not an automatic open.
- T04/T05 flip the line/point/wireframe entries from `primitive` to `opened`
  with `lines` or `points`. A later task that changes an importer must update
  the owning manifest row in the same change.
- Real-world samples that hit a resource ceiling stay `failed`/`resource` with
  a named owner (currently `real-cubegears-3mf` → T09); they are measured
  gaps, not hidden passes.
- The scoreboard does not emulate a user's Open As choice, so it cannot claim
  that a renamed file opens without that choice. This is deliberate: the
  product's gate is the behaviour under test.

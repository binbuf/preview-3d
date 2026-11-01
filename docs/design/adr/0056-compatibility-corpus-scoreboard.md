# 0056 — Compatibility corpus outcomes are broker observations, not parser claims

Status: accepted

## Context

0.7.0 adds formats and best-effort compatibility. Without a shared scoreboard,
"it opens now" is anecdotal, and a parser-side success can hide a host-side
failure or a silent feature drop. The existing Preview3D corpus already covers
glTF, STL, PLY, compressed glTF, unsafe sidecars and one STEP part, but each
test asserted its own shape.

## Decision

`tests/compat/manifest.json` (schema 1) is the single scoreboard. Every entry
names a repository-relative fixture, a format, an expected outcome
(`opened`/`degraded`/`failed`), the typed `ImportErrorCode` on failure, and
optional warning identifiers. The `[compat]` test runs each entry through
`import_broker::RunImportSession` — the real worker or dedicated host — and
derives the outcome from the broker result:

- `opened` requires published geometry and no compatibility warning.
- `degraded` requires published geometry and at least one warning; success
  without a warning is not degraded.
- `failed` requires the exact typed error code.

Entries that need an absent host executable report `skipped` in the evidence
report rather than crashing or failing a worker-only build. The hidden
`[write-compat-report]` case writes `artifacts/compat/<Configuration>.json`
containing only manifest ids and measured facts; the checked assertion stays
the `[compat]` test.

Breadth tasks append entries and may change an expectation only as part of an
importer change they own. T01 records the baseline; it does not fix failures.

## Consequences

- Later tasks get one open-rate table and stable fixture ids to name in
  hand-offs.
- The corpus needs a small in-test JSON reader because the test binary has no
  general JSON dependency; the schema stays deliberately narrow.
- New public warning categories must be mapped to manifest identifiers when
  they cross the wire.
- A green `[compat]` run means every recorded outcome still matches. A changed
  outcome is visible as a named fixture and typed error, not a silent pass.

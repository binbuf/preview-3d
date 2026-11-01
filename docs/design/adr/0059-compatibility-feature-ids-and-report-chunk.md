# 0059 — Compatibility feature IDs and the report chunk

- Status: Accepted
- Context: tasks T04+ (0.7.0)

## Context

`ImportStatusPayload::optionalFeatureWarnings` is only a bounded count. The
host cannot say which optional feature was degraded or ignored, so users
cannot judge whether a preview is faithful and support cannot classify a
failure. The wire format must remain fixed-size, string-free, checksum-covered
and safe against a hostile worker.

## Decision

Add an optional, generation-scoped `ChunkTopology::CompatibilityReport` (10)
with fixed chunk ID `0xf0000003`. Its 776-byte payload holds `recordCount`
(0–64), a zero reserved word, and 64 fixed 12-byte records of
`{featureId, severity, reserved[3], count}`. The validator requires the exact
size, at most one report per generation, zero padding/dependencies/reserved
bytes, severity in `{Critical, Degraded, Ignored}`, counts in
`[1, 1,000,000]`, and a feature ID inside a reserved range:

| Range | Owner |
| --- | --- |
| 1–999 | glTF extensions/features |
| 1000–1999 | 3MF |
| 2000–2999 | USD |
| 3000–3999 | STEP |
| 4000–4999 | textures (any format) |
| 5000–5999 | containers/archives (any format) |

Producers deduplicate by `(featureId, severity)` and saturate counts in a
shared `CompatibilityReportBuilder`. The worker never sends text; the trusted
host maps IDs to localized strings (`CompatibilityText.h`). The legacy count
and its generic warning remain until every adapter migrates. The topology is
additive, so protocol version 10 and the section layout stay unchanged.

## Consequences

- Later format tasks append named IDs inside their reserved range and never
  renumber; unknown glTF extensions collapse into one generic bucket because
  the worker may not send the extension string.
- `Critical` is diagnostic only and never replaces the typed error code.
- Adding a new feature ID requires a host localization entry (or deliberately
  falls back to the generic warning).
- Hosts that predate the chunk ignore/reject it per the ordinary unknown-
  topology rule; they still get the legacy count.

---
timeoutMin: 300
verify: x64\Release\Tests.Unit.exe "[compatibility]"
---
# T04 — Compatibility severity model and report chunk

## Goal
The worker/host can report **which** optional features were degraded or ignored,
with a stable numeric feature ID and severity, instead of only incrementing
`optionalFeatureWarnings`. The host maps those IDs to specific localized text,
so users know whether a preview is faithful and support can classify a failure.

## Context (read first)
- `docs/proposal.md` §3.2 and Appendix D — severity model and glTF policy.
- `shared/model-core/include/model_core/WireFormat.h:40-51` — chunk topology.
- `shared/model-core/include/model_core/WireFormat.h:107-118` —
  `ImportStatusPayload` (flags/counts; no strings).
- `shared/import-broker/src/SharedSectionValidator.cpp:266-330` — descriptor
  validation; every new topology needs an explicit rule.
- `interactive-viewer/src/app/RenderThread.cpp:61-73` — current generic warning
  composition.
- `interactive-viewer/src/app/Preview3D.cpp:2015-2017` — generic STEP warning.
- `docs/design/03-file-formats-and-ingestion.md` § Error taxonomy.

## Scope
- [x] Add `shared/model-core/include/model_core/Compatibility.h`:
  - `CompatibilitySeverity { Critical, Degraded, Ignored }`;
  - `FeatureId` numeric ranges per format (glTF extension, 3MF extension,
    USD schema/primitive, STEP header/unit feature, texture/container);
  - a fixed-size `CompatibilityRecord { uint32_t featureId; uint8_t severity;
    uint8_t reserved[3]; uint32_t count; }`;
  - `CompatibilityReportPayload` with a bounded `recordCount` and a fixed array
    (e.g. 64 records), no strings/paths.
- [x] Add `ChunkTopology::CompatibilityReport = 10` and validator rules:
  exact payload size, `recordCount <= max`, reserved bytes zero, severity in
  range, feature ID in a known/enumerated range, no dependencies, bounded
  counts. A report chunk may appear at most once per generation.
- [x] Add a helper in `BoundedChunkWriter` or a small adapter helper that
  deduplicates feature IDs, saturates counts and emits the chunk.
- [x] Change glTF’s existing `optionalFeatureWarnings` counting to emit stable
  records for every unsupported `extensionsUsed` entry, without changing
  parsing behavior yet. Keep the old count for compatibility.
- [x] Add host-side aggregation in `RenderThread`/`ModelData`:
  `CompatibilityRecord` list, plus a `RebuildModelWarning` that appends specific
  localized messages (one per degraded feature, capped) and keeps the generic
  fallback if none are known.
- [x] Add a host-side `FeatureId -> localized text` table in the UI layer (or a
  shared product-owned table with localization keys). Never accept worker text.
- [x] Extend diagnostics to list feature IDs/severities/counts; no source path.
- [x] Tests:
  - wire round-trip/validator accepts a valid report and rejects bad
    count/size/severity/dependency;
  - a glTF fixture with `KHR_materials_clearcoat` in `extensionsUsed` produces a
    `Degraded` record and specific warning text;
  - `[compatibility]` unit and import-isolation cases.
- [x] Update `docs/design/03-file-formats-and-ingestion.md` with the new chunk
  and feature-ID policy.

## Out of scope
- Actually enabling/degrading the glTF extensions (→ T05).
- 3MF/USD/STEP feature semantics (their tasks add records).
- New material lobes or renderer behaviour (→ T18).
- A user-visible full compatibility report panel; this task only feeds the
  existing warning/diagnostics surface.

## Design notes
- The worker must not send arbitrary text. Feature IDs are stable numeric
  constants; localization lives in the trusted host.
- `Critical` features normally fail the import; the report is primarily for
  `Degraded`/`Ignored`. If a critical feature is reported before a typed
  failure, it is diagnostic only and must not change the error code.
- Deduplicate by `featureId + severity`; saturate `count` at `UINT32_MAX` or a
  lower documented cap. Do not let a hostile worker create unbounded records.
- The report is part of the generation; it must be checksum-covered and
  validated like any other payload. Do not add a side channel.
- Preserve the existing generic `optionalFeatureWarnings` text for older
  adapters until each is migrated.

## Done when
- [x] `x64\Release\Tests.Unit.exe "[compatibility]"` passes.
- [x] `x64\Release\Tests.ImportIsolation.exe "[compatibility]"` passes.
- [x] A glTF with a visual-only extension produces a specific warning, not the
  generic "some optional features" text.
- [x] `SharedSectionValidator` rejects a truncated/oversized/malformed report
  and the hostile-worker suite covers the new topology.
- [x] Wire-format docs and the protocol fixture(s) are updated.
- [x] Hand-off lists the feature-ID ranges reserved for 3MF/USD/STEP.

## Hand-off

Implemented the full scope.

**Wire layout.** `ChunkTopology::CompatibilityReport = 10`, fixed chunk ID
`0xf0000003`. Payload `CompatibilityReportPayload` is 776 bytes:
`recordCount u32` (0–64), `reserved u32` (zero), `records[64]` of
`CompatibilityRecord` (12 bytes: `featureId u32`, `severity u8`,
`reserved[3]` zero, `count u32`). Severity is `Critical=0`, `Degraded=1`,
`Ignored=2`; counts are capped/saturated at `1'000'000`. Header is in
`shared/model-core/include/model_core/Compatibility.h`. The topology is
additive: protocol stays **10**, no cache or section-layout bump.

**Producer.** `CompatibilityReportBuilder` dedups by `(featureId, severity)`
and saturates; `BoundedChunkWriter::AddCompatibilityReport` emits the chunk.
The glTF adapter now emits a `Degraded` record for each unsupported
`extensionsUsed` entry while keeping the legacy `optionalFeatureWarnings`
count unchanged.

**Validator.** Exact size, fixed ID, once per generation (also enforced by
`ImportSession`), no dependencies, zero reserved/padding bytes, severity in
range, known feature range, count `[1, 1'000'000]`.

**Host.** `ImportResult`/`ModelData` carry the records; `RenderThread`
merges batches and `CompatibilityWarningText`
(`interactive-viewer/src/app/CompatibilityText.h`) shows up to four specific
localized messages, falling back to the generic text only when no ID has host
text. `DiagnosticDetails` appends feature ID/severity/count with no source
path.

**Feature-ID ranges reserved for later tasks.** glTF 1–999 (3MF
1000–1999, USD 2000–2999, STEP 3000–3999, texture 4000–4999,
container 5000–5999). Named glTF IDs occupy 1–8; unknown glTF extensions use
900. Do not renumber; later tasks append within their range.

**Docs.** Updated `docs/design/03-file-formats-and-ingestion.md` and added
`docs/design/adr/0059-compatibility-feature-ids-and-report-chunk.md`.

**Checks.**
- `x64\Release\Tests.Unit.exe "[compatibility]"` — 145 assertions, 7 cases, pass.
- `x64\Release\Tests.ImportIsolation.exe "[compatibility]"` — 93 assertions, 10 cases, pass (includes the `KHR_materials_clearcoat` GLB and seven hostile-worker report modes).
- `[wire-format]` 49 assertions pass; `[gltf-import]` 648 pass; `[recovery]` 2319 pass (3 fault-harness skips); `[chunk-batch]` 99 pass; `[compat]` 312 pass.
- `Preview3D.slnx /t:Preview3D` Release builds.

No deviations or blockers. Follow-ups for T05/T07/T10b/T11/T12/T13 are in
`docs/0.7.0/progress/T04.md`.

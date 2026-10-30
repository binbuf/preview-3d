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
- [ ] Add `shared/model-core/include/model_core/Compatibility.h`:
  - `CompatibilitySeverity { Critical, Degraded, Ignored }`;
  - `FeatureId` numeric ranges per format (glTF extension, 3MF extension,
    USD schema/primitive, STEP header/unit feature, texture/container);
  - a fixed-size `CompatibilityRecord { uint32_t featureId; uint8_t severity;
    uint8_t reserved[3]; uint32_t count; }`;
  - `CompatibilityReportPayload` with a bounded `recordCount` and a fixed array
    (e.g. 64 records), no strings/paths.
- [ ] Add `ChunkTopology::CompatibilityReport = 10` and validator rules:
  exact payload size, `recordCount <= max`, reserved bytes zero, severity in
  range, feature ID in a known/enumerated range, no dependencies, bounded
  counts. A report chunk may appear at most once per generation.
- [ ] Add a helper in `BoundedChunkWriter` or a small adapter helper that
  deduplicates feature IDs, saturates counts and emits the chunk.
- [ ] Change glTF’s existing `optionalFeatureWarnings` counting to emit stable
  records for every unsupported `extensionsUsed` entry, without changing
  parsing behavior yet. Keep the old count for compatibility.
- [ ] Add host-side aggregation in `RenderThread`/`ModelData`:
  `CompatibilityRecord` list, plus a `RebuildModelWarning` that appends specific
  localized messages (one per degraded feature, capped) and keeps the generic
  fallback if none are known.
- [ ] Add a host-side `FeatureId -> localized text` table in the UI layer (or a
  shared product-owned table with localization keys). Never accept worker text.
- [ ] Extend diagnostics to list feature IDs/severities/counts; no source path.
- [ ] Tests:
  - wire round-trip/validator accepts a valid report and rejects bad
    count/size/severity/dependency;
  - a glTF fixture with `KHR_materials_clearcoat` in `extensionsUsed` produces a
    `Degraded` record and specific warning text;
  - `[compatibility]` unit and import-isolation cases.
- [ ] Update `docs/design/03-file-formats-and-ingestion.md` with the new chunk
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
- [ ] `x64\Release\Tests.Unit.exe "[compatibility]"` passes.
- [ ] `x64\Release\Tests.ImportIsolation.exe "[compatibility]"` passes.
- [ ] A glTF with a visual-only extension produces a specific warning, not the
  generic "some optional features" text.
- [ ] `SharedSectionValidator` rejects a truncated/oversized/malformed report
  and the hostile-worker suite covers the new topology.
- [ ] Wire-format docs and the protocol fixture(s) are updated.
- [ ] Hand-off lists the feature-ID ranges reserved for 3MF/USD/STEP.

## Hand-off
_(filled in by the implementing session: exact payload layout, feature-ID
ranges, host localization table, and any protocol/cache version bump.)_

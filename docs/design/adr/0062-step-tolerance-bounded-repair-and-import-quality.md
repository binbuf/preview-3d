# 0062 — STEP tolerance, bounded repair and import quality

## Status
accepted

## Context
T07 addresses three self-inflicted failures in the bounded STEP host: a raw
substring check rejected any file mentioning `FILE_POPULATION`/`DOCUMENT_FILE`
(even inside comments/strings), a missing length unit failed as
`UnsupportedRequiredFeature`, and a definition that OCCT could transfer but not
tessellate had no recovery path. The host also had no way to expose the existing
STEP-004 deflection policy as a user-visible quality choice. The host holds no
path authority, so none of these changes may resolve an external document.

## Decision
- **External documents.** Admission counts `FILE_POPULATION`/`DOCUMENT_FILE`
  only when it is the Part-21 entity name, outside comments and quoted strings.
  The default cap is 64 declarations; exceeding it is the existing typed
  `ExternalDocument` failure. The main document is always attempted. Unresolved
  declarations are emitted as `CompatibilitySeverity::Degraded` feature ID 3000
  and never opened. A main document with no transferable roots still fails
  `EmptyGeometry`. The thumbnail provider keeps its stricter self-contained
  policy by setting the cap to zero.
- **Units.** A missing/unresolvable length unit assumes millimetres
  (`0.001` metre per transferred unit) and warns with feature ID 3001.
  More than one authored length-unit entity uses the reader's first/dominant
  factor, warns with feature ID 3002, and never claims the authored unit was
  exact. A non-finite/non-positive factor takes the millimetre assumption; a
  positive factor above `1e12` keeps the existing typed overflow failure.
- **Repair.** `StepHealingPolicy::BoundedRepairOnFailure` is the release policy.
  `ShapeFix_Shape` runs per definition only when the normal mesh/extract pass
  produced no triangles for a mesh-related failure; budget/cancellation failures
  are never repaired. The repair budget is 20,000 faces, 200,000 edges and 30 s
  per definition, checked before/after the call. A repaired shape must pass
  `BRepCheck_Analyzer` and still produce geometry; success is published as
  `Degraded` feature ID 3003, failure preserves the original typed outcome.
  Definitions that mesh but leave unmeshed faces are reported as
  `Ignored` feature ID 3004.
- **Import quality.** The STEP request carries a closed two-bit field:
  Balanced (both bits clear) is the default and keeps the STEP-004 display
  constants; Draft uses the former coarse constants; Full adds finer relative
  and angular deflection inside the same absolute window and budgets.
  `kStepTessellationProfileVersion` is now 6. `StepImporterCacheIdentity`
  folds the quality into the importer version so Draft/Balanced/Full can never
  share cached normalized bytes.

## Consequences
- Valid files stop failing for header-only reasons while malformed/unsafe files
  keep their typed outcomes; no path/network authority was added.
- Cache invalidation is explicit: the profile version bump (5 → 6) invalidates
  every existing STEP cache entry, and the quality field keeps new entries
  separate.
- Repair can change emitted geometry for an otherwise-unusable definition, so
  it is always surfaced as degraded; a file that repairs identically on every
  run is still deterministic because `ShapeFix` is called with fixed defaults.
- The provider's STEP thumbnail path remains self-contained and unchanged; its
  beacon API still reports `ExternalDocumentRejected`.

---
timeoutMin: 300
verify: x64\Release\Tests.ImportIsolation.exe "[step-tolerance]"
---
# T07 — STEP tolerance and import quality

## Goal
Valid STEP files stop failing for header/unit reasons: declared external
documents are attempted with a warning instead of a preflight veto, a missing
length unit defaults to millimetres with a warning, and an opt-in bounded repair
path exists for geometry that OCCT can transfer but not tessellate. A
Draft/Balanced/Full import-quality setting controls deflection without changing
the default.

## Context (read first)
- `docs/proposal.md` §3.3 — external documents, units, healing, import quality.
- `compatibility-host-step/src/StepPart21Preflight.cpp:350-430` — scanner state
  and the current raw-text `FILE_POPULATION`/`DOCUMENT_FILE` rejection.
- `compatibility-host-step/src/StepXdeAdapter.cpp:1475-1503` — `FileUnits` and
  `GetLengthUnit` strictness.
- `compatibility-host-step/src/StepTessellationProfile.h:35-165` — profile
  version, deflection constants and `StepHealingPolicy::None`.
- `compatibility-host-step/src/StepXdeAdapter.cpp:1146-1177, 1259-1260` —
  per-definition meshing and budget failures.
- `shared/model-core/include/model_core/ControlProtocol.h` — request flags and
  versioning for an import-quality option.
- `docs/design/adr/0017-*` (STEP self-contained decision) and
  `docs/design/03-file-formats-and-ingestion.md` — must be amended.

## Scope
- [ ] Replace the raw substring external-document check with an entity-aware
  scan that only examines Part-21 instance names outside comments and strings.
  A `FILE_POPULATION`/`DOCUMENT_FILE` declaration becomes a bounded warning,
  not a preflight failure.
- [ ] Attempt the main document with OCCT even when external declarations exist.
  If the main document has no transferable roots, fail typed; if it has usable
  geometry and externals are unresolved, publish it with a compatibility
  warning. Never open the external path (the host has no path authority).
- [ ] Units:
  - absent `FileUnits` / unresolvable `GetLengthUnit` → assume millimetres
    (metre factor 0.001), add a warning, and record the assumption;
  - multiple contradictory units → choose the first/dominant unit, convert what
    can be converted deterministically, and add a warning; do not claim exact
    authored units;
  - keep the current positive-factor/overflow validation.
- [ ] Healing:
  - add `StepHealingPolicy::BoundedRepairOnFailure` (or equivalent) to the
    profile, with a time/face/edge budget;
  - run only after read/transfer/tessellation fails, never by default on valid
    geometry;
  - if repair succeeds, mark the result `Degraded`/warning and bump
    `kStepTessellationProfileVersion`;
  - if repair fails, preserve the existing typed error.
- [ ] Import quality:
  - add `Draft | Balanced | Full` (or equivalent) to the STEP request/options;
  - map to existing relative/absolute deflection constants without changing the
    default (Balanced = current values);
  - include the quality in STEP importer/cache identity;
  - keep all budgets and deterministic output.
- [ ] Add fixtures (small, licensed or generated):
  - STEP with `FILE_POPULATION`/`DOCUMENT_FILE` but self-contained main geometry;
  - STEP with no unit;
  - STEP with two contradictory units;
  - STEP needing a small repair, plus an unrepairable one;
  - the existing STEP positive corpus at Draft/Balanced/Full.
- [ ] Add `[step-tolerance]` cases asserting outcome, warnings, unit factor and
  that malformed/unsafe files still fail typed.
- [ ] Update `docs/design/01-product-scope.md`, `docs/design/03-file-formats-and-ingestion.md`,
  `docs/FORMAT-SUPPORT.md` and the ADR that currently says STEP is
  self-contained only.

## Out of scope
- IGES/BREP/XBF/FCStd (→ T08/T09).
- Wireframe/line output (→ T17).
- Exact measurement, PMI/GD&T, shape healing beyond preview repair, external
  STEP document *resolution* (we warn/skip; we do not broker those files).
- Changing the job/commit/time ceilings.

## Design notes
- “Assume millimetres” is a display/units default, not a claim about the file.
  The UI/diagnostics must say the unit was assumed.
- External documents remain **unresolved**. The task must not add path access to
  the STEP host; it only stops rejecting the main document because a reference
  exists.
- Repair must be deterministic and bounded; record the profile version bump so
  cached output cannot be reused across the policy change.
- Import quality is part of the cache key and importer version; two qualities
  must not share cached normalized bytes.
- Do not raise per-definition budgets to make a failing file pass; that is a
  separate limit change/ADR.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe "[step-tolerance]"` passes.
- [ ] The external-document fixture opens with a warning; the no-unit fixture
  opens with the millimetre assumption; contradictory units warn and pick a
  documented value.
- [ ] A repair-needed fixture either opens degraded with a warning or fails with
  the same typed error if repair is impossible; no silent mutation of valid
  geometry.
- [ ] Draft/Balanced/Full produce deterministic, distinct normalized outputs
  and cache identities.
- [ ] No path/network access was added to the STEP host.
- [ ] Docs/ADRs updated; hand-off lists the profile version and quality mapping.

## Hand-off
_(filled in by the implementing session: external-document policy, unit
assumption, repair algorithm/budget, quality mapping, fixtures and any residual
failures.)_

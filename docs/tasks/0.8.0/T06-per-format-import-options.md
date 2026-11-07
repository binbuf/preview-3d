---
timeoutMin: 300
verify: x64\Release\Tests.ImportIsolation.exe "[import-options]"
---
# T06 — Per-format import options and quality surface

## Goal
Expose the import controls the adapters already need, instead of hiding them
behind a single default. Users must be able to turn CAD wire reading on or off,
choose Draft/Balanced/Full CAD quality, adjust deflection, override a missing
or wrong unit, and select a USD subdivision level. Options must be validated,
included in cache identity, and visible in the UI/CLI.

## Context (read first)
- `shared/model-core/include/model_core/ControlProtocol.h` — the existing
  STEP quality flag bits (`kImportRequestStepQualityDraft`/`Full`) and the
  closed `StepImportQuality` mapping.
- `shared/import-broker/include/import_broker/ImportSession.h` —
  `ImportSessionRequest::stepImportQuality` and the other per-session fields.
  Nothing in `interactive-viewer/` currently sets the quality field.
- `compatibility-host-step/src/StepTessellationProfile.h` and
  `StepXdeAdapter.cpp` — the versioned deflection profile and the `read_wire`
  behavior added by T05.
- `interactive-viewer/src/app/Preview3D.cpp` and `Settings.cpp` — the CLI and
  persisted settings surfaces.
- `shared/model-core/include/model_core/CacheIdentity.h` and the import cache
  identity — options that change normalized output must change the identity.
- F3D reference: `doc/user/02-SUPPORTED_FORMATS.md:76-104` exposes
  `STEP.linear_deflection`, `angular_deflection`, `relative_deflection`,
  `read_wire`, `IGES.*`, `BREP.*`, `XBF.*`, `FCStd.*`, and
  `USD.subdivision_level`. Use the shape of that surface, not its runtime.

## Scope
- [ ] Define a closed, validated `ImportOptions` value shared by the viewer,
  broker, worker, and hosts. At minimum:
  - `readWire` (bool) for STEP/IGES/BREP/FCStd;
  - `quality` (`Draft` | `Balanced` | `Full`) for all CAD formats;
  - `linearDeflection`, `angularDeflection`, `relativeDeflection` with
    bounded ranges and format-specific defaults;
  - `unitScaleOverride` (finite positive, optional) for formats with missing or
    contradictory units;
  - `usdSubdivisionLevel` (bounded unsigned) for the OpenUSD host.
- [ ] Replace or extend the STEP-only quality bits with a generic option
  record. Preserve the existing Balanced default and every existing caller.
  Reject unknown option names, out-of-range values, NaN/Inf, and options that
  do not apply to the selected format.
- [ ] Add a repeatable CLI form, for example
  `--import-option <format>.<name>=<value>` (case-insensitive format/name),
  plus a Settings surface for the same options. Persist only validated values.
- [ ] Apply the options in the broker/session and host request. The worker and
  host still validate every value; the trusted process is not the only guard.
- [ ] Include every option that changes normalized output in cache identity.
  `readWire` and quality/deflection/unit/subdivision must not reuse a cache
  entry produced with different options.
- [ ] Add `[import-options]` cases:
  - parser accepts valid values case-insensitively and rejects unknown names,
    non-finite values, and out-of-range values;
  - `readWire=0` suppresses edge-only fallback/overlay; `readWire=1` includes
    edges alongside faces for a solid fixture;
  - Draft vs Full changes triangle counts or deflection deterministically;
  - unit override changes reported bounds by the expected factor;
  - USD subdivision level changes output or is rejected when unsupported;
  - cache identity differs for every output-changing option.
- [ ] Update `docs/FORMAT-SUPPORT.md`, `README.md`, and the task README with the
  exact option names, defaults, ranges, and formats.

## Out of scope
- Arbitrary reader-supplied option strings or plug-in options. The set is
  closed and product-owned.
- Exposing third-party parser internals (Assimp properties, OCCT parameters
  beyond the documented deflection controls).
- Animation time, camera selection, or lighting controls.
- Changing the default presentation for any format; options are opt-in except
  the existing Balanced default.

## Design notes
- Keep the wire/control protocol versioned. Adding fields to an existing
  request is a breaking change unless the record is explicitly replaced and
  every producer/consumer moves together; follow the existing protocol rules.
- A unit override is a user assertion, not a silent guess. It must be finite,
  positive, bounded, and recorded in diagnostics. Do not let it bypass the
  existing unit checks for a format that authors a valid unit.
- `read_wire` should default to the T05 fallback behavior (edges only when no
  faces) for compatibility. An explicit `read_wire=1` may add edges alongside
  faces; `read_wire=0` must suppress both the fallback and the overlay.
- The cache identity must include the format, importer, options, and the
  existing source identity. Do not add a path or user text to the identity.
- Keep the UI simple: a per-format “Import options” section with reset-to-
  defaults, not a raw key/value box.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe "[import-options]"` passes.
- [ ] `x64\Debug\Tests.ImportIsolation.exe "[import-options]"` passes.
- [ ] STEP quality is reachable from the UI/CLI and changes the output.
- [ ] `read_wire`, deflection, unit override, and USD subdivision are
  validated, applied, and reflected in cache identity.
- [ ] Invalid/unknown options fail typed before the parser runs.
- [ ] Docs list every option and default; no undocumented option is accepted.

## Hand-off
_(filled in by the implementing session: option schema, protocol/cache changes, UI surface, defaults, and the exact tests that prove each option)_

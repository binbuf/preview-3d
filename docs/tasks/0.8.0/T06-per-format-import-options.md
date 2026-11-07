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
- [x] Define a closed, validated `ImportOptions` value shared by the viewer,
  broker, worker, and hosts. At minimum:
  - `readWire` (bool) for STEP/IGES/BREP/FCStd;
  - `quality` (`Draft` | `Balanced` | `Full`) for all CAD formats;
  - `linearDeflection`, `angularDeflection`, `relativeDeflection` with
    bounded ranges and format-specific defaults;
  - `unitScaleOverride` (finite positive, optional) for formats with missing or
    contradictory units;
  - `usdSubdivisionLevel` (bounded unsigned) for the OpenUSD host.
- [x] Replace or extend the STEP-only quality bits with a generic option
  record. Preserve the existing Balanced default and every existing caller.
  Reject unknown option names, out-of-range values, NaN/Inf, and options that
  do not apply to the selected format.
- [x] Add a repeatable CLI form, for example
  `--import-option <format>.<name>=<value>` (case-insensitive format/name),
  plus a Settings surface for the same options. Persist only validated values.
- [x] Apply the options in the broker/session and host request. The worker and
  host still validate every value; the trusted process is not the only guard.
- [x] Include every option that changes normalized output in cache identity.
  `readWire` and quality/deflection/unit/subdivision must not reuse a cache
  entry produced with different options.
- [x] Add `[import-options]` cases:
  - parser accepts valid values case-insensitively and rejects unknown names,
    non-finite values, and out-of-range values;
  - `readWire=0` suppresses edge-only fallback/overlay; `readWire=1` includes
    edges alongside faces for a solid fixture;
  - Draft vs Full changes triangle counts or deflection deterministically;
  - unit override changes reported bounds by the expected factor;
  - USD subdivision level changes output or is rejected when unsupported;
  - cache identity differs for every output-changing option.
- [x] Update `docs/FORMAT-SUPPORT.md`, `README.md`, and the task README with the
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
- [x] `x64\Release\Tests.ImportIsolation.exe "[import-options]"` passes.
- [x] `x64\Debug\Tests.ImportIsolation.exe "[import-options]"` passes.
- [x] STEP quality is reachable from the UI/CLI and changes the output.
- [x] `read_wire`, deflection, unit override, and USD subdivision are
  validated, applied, and reflected in cache identity.
- [x] Invalid/unknown options fail typed before the parser runs.
- [x] Docs list every option and default; no undocumented option is accepted.

## Hand-off

### Option schema and defaults

One `model_core::ImportOptions` record in
`shared/model-core/include/model_core/ImportOptions.h`:

| Format(s) | Name | Default | Range |
| --- | --- | --- | --- |
| STEP, IGES, BREP, XBF, FCStd | `quality` | `balanced` | `draft`, `balanced`, `full` |
| STEP, IGES, BREP, XBF, FCStd | `read_wire` | fallback | absent = T05 fallback; `0` = never; `1` = always |
| STEP, IGES, BREP, XBF, FCStd | `linear_deflection` | quality profile | finite `> 0`; `<= 0.5` relative or `<= 1e6` absolute |
| STEP, IGES, BREP, XBF, FCStd | `angular_deflection` | quality profile | finite `> 0` and `<= pi` radians |
| STEP, IGES, BREP, XBF, FCStd | `relative_deflection` | `true` | `0`/`1` |
| STEP, IGES, BREP, XBF, FCStd | `unit_scale` | format resolution | finite `> 0` and `<= 1e9` metres/unit |
| USD | `subdivision_level` | `0` | `0..4`; `> 0` is a typed `UnsupportedRequiredFeature` in this build |

Format/name matching is ASCII case-insensitive. Unknown formats/names,
non-finite or out-of-range values, and options that do not apply to the format
fail typed before any parser process starts. `read_wire=1` adds edges alongside
faces; `read_wire=0` suppresses the T05 fallback and the overlay. `unit_scale`
runs the format's authored-unit checks first, then replaces `metersPerUnit` and
records `kCadFeatureUnitOverrideApplied` (3018). `quality` maps to the versioned
STEP tessellation profile (v8); explicit deflections override the profile value
inside the same absolute window. `relative_deflection=0` without an explicit
linear value uses the quality-mapped absolute defaults (0.5/0.1/0.02).

### Protocol and cache changes

- `ImportOptionsPayload` (48 bytes) is appended to `ParseStepFileRequest`,
  `ParseCadFileRequest`, `ParseUsdFileRequest` and `ParseOpenUsdFileRequest`.
  Sizes are now 96/104/96/96. `version == 0` keeps every pre-T06 zero-filled
  request valid; the existing STEP quality flag bits remain authoritative for
  legacy quality.
- `ImportSessionRequest::importOptions` is optional; the legacy
  `stepImportQuality` still works. `ImportSessionResult` exposes
  `importOptionsCacheIdentity`.
- `model_core::ImportOptionsCacheIdentity(format, options)` hashes format,
  quality, wire mode, deflections, relative mode, unit override and subdivision.
  `StepImporterCacheIdentity` mixes it with `kStepImporterVersion`;
  `CacheEntryKey` now carries `format` and `importOptionsIdentity`, and the
  cache schema is v5.
- The broker validates before launching (`ImportStage::ValidateOptions`); the
  STEP/CAD host, USD fast worker and OpenUSD host re-validate the packed form.

### UI/CLI surface

- Repeatable `--import-option <FORMAT>.<name>=<value>` and
  `--import-option=<FORMAT>.<name>=<value>`.
- **Settings → Import options…** native popup with CAD quality, CAD wires,
  CAD deflection presets, CAD unit override, USD subdivision level, and
  reset-to-defaults.
- `settings.json` keys `import.<FORMAT>.<name>` are written only for validated
  non-default values; invalid persisted values are ignored on load.

### Tests and commands

- `x64\Release\Tests.ImportIsolation.exe "[import-options]"` — 192 assertions,
  10 test cases, passed.
- `x64\Debug\Tests.ImportIsolation.exe "[import-options]"` — 192 assertions,
  10 test cases, passed.
- `[cad-wires]` 486 assertions, `[valid-file]` 163 assertions,
  `[step-003],[step-004],[step-005],[step-006],[step-tolerance],[cad-iges],
  [cad-brep],[cad-xbf],[cad-fcstd],[cad-validator],[topology],
  [valid-file-self-check]` 1047 assertions, `[compat]` 600 assertions, and
  `[usd-009],[usd-primitives]` 194 assertions all passed in Release.
- `x64\Release\Tests.Unit.exe "[cache]"` — 91 assertions, 10 test cases,
  passed.
- `Preview3D.vcxproj` Release builds; `Tests.Unit.vcxproj` Release builds.

### Changed docs

`docs/FORMAT-SUPPORT.md`, `README.md`, `docs/tasks/0.8.0/README.md`,
`docs/design/03-file-formats-and-ingestion.md`, and
`docs/design/adr/0100-generic-per-format-import-options.md`.

### Remaining work / notes

- USD subdivision levels above 0 are validated, routed and cache-keyed but
  rejected as unsupported because this build links no subdivision evaluator.
  A later task can implement the evaluator without changing the grammar.
- The UI applies CAD changes to every CAD format because the product runs one
  CAD host policy; the CLI and Settings JSON remain per-format.
- The derived cache is still not wired into the viewer; the identity changes
  are proven through `ImportOptionsCacheIdentity`, `StepImporterCacheIdentity`
  and `CacheEntryKey`.

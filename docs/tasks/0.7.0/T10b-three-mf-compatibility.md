---
timeoutMin: 240
verify: x64\Release\Tests.ImportIsolation.exe "[3mf-compat]"
---
# T10b — 3MF best-effort compatibility

## Goal
Real slicer-exported 3MF files open more often: unknown optional namespaces are
ignored, a triangle whose corners use different textures/samplers is split or
degraded instead of failing the whole model, and an unsupported required
extension fails only when the visible geometry actually depends on it.

## Context (read first)
- `docs/proposal.md` §3.2 — best-effort compatibility policy.
- `import-worker/src/ThreeMfDisplayProperties.cpp:70-98` —
  `CheckRequiredExtensions` currently accepts only the Core, Materials,
  Production, Beam and Ball namespaces; any other required prefix is fatal.
- `import-worker/src/ThreeMfAdapter.cpp:126-195` — object/component traversal
  and `AllowedType`.
- `import-worker/src/ThreeMfAdapter.cpp:370-395` — per-corner texture/sampler
  mismatch currently returns `UnsupportedRequiredFeature`.
- `import-worker/src/ThreeMfOpcPreflight.cpp` — archive/OPC admission.
- `shared/model-core/include/model_core/Compatibility.h` — T04 report.
- F3D/real-world 3MF fixtures from PrusaSlicer, Bambu Studio, Cura and
  OrcaSlicer; verify licenses before committing.

## Scope
- [x] Split `CheckRequiredExtensions` into:
      - known/understood namespaces (current allowlist);
      - unknown namespaces: record an `Ignored`/`Degraded` compatibility feature
        and continue if core geometry is usable;
      - a required extension that alters visible geometry and has no fallback:
        still `UnsupportedRequiredFeature`, with the feature ID.
- [x] Per-corner material variation:
      - if a triangle’s corners differ in texture/sampler/texture-layer/mix,
        split the triangle into per-material triangles when doing so stays
        within the triangle/material/chunk budgets;
      - otherwise choose the dominant corner with a `Degraded` warning; do not
        fail the model;
      - keep deterministic material ordering.
- [x] Preserve the existing allowances for slicer-private object types
      (including `other` referenced by the build) and the bounded Beam Lattice
      fallback.
- [x] Add compatibility records for ignored required extensions, degraded
      per-corner splits and unsupported-but-not-geometry-critical features.
- [x] Add fixtures:
      - a PrusaSlicer/Bambu/Cura file with private metadata namespaces but no
        required geometry impact;
      - a model with per-corner texture/sampler variation;
      - a file with a genuinely unsupported required extension that affects
        visible geometry (must fail);
      - malformed/over-budget OPC cases retained.
- [x] Add `[3mf-compat]` cases asserting outcome, warning feature IDs, split
      triangle/material counts and that malformed/unsafe files still fail.
- [x] Update `docs/FORMAT-SUPPORT.md` and the 3MF support notes.

## Out of scope
- Using Assimp’s 3MF importer as a fallback (T10 explicitly excludes 3MF; file a
  follow-up only if the lib3mf policy proves insufficient).
- Slicer plate grouping/print settings, Slice/Secure Content/Volumetric/
  Implicit/toolpath semantics.
- New 3MF geometry types beyond the current supported static subset.

## Design notes
- “Unknown required” is not automatically fatal: the question is whether the
  visible geometry depends on it. Core mesh/build data remains authoritative
  when present.
- Splitting triangles changes material grouping but not positions/UVs; keep
  bounds and normalized geometry identical.
- If splitting would exceed the material/triangle/chunk cap, degrade to the
  dominant material rather than returning `ResourceLimit` when a usable preview
  exists.
- Never ignore a required extension that replaces the mesh or changes its
  topology; those remain terminal.
- Keep the OPC preflight and archive limits unchanged; this task changes model
  semantics, not the ZIP trust boundary.

## Done when
- [x] `x64\Release\Tests.ImportIsolation.exe "[3mf-compat]"` passes.
- [x] The slicer-metadata fixture opens; the per-corner fixture opens with
      split triangles or a documented degraded choice.
- [x] An unsupported required geometry extension still fails typed.
- [x] Warning IDs/severities are visible through T04.
- [x] Archive/OPC limits and hostile-worker cases remain green.
- [x] Docs updated; no new limit was added without an ADR.
- [x] Hand-off records the required-extension policy and split/degrade rule.

## Hand-off

### What landed
- `CheckRequiredExtensions` now classifies required namespaces instead of
  rejecting everything outside the old allowlist. Supported: Core, Materials,
  Production, Beam, Ball. Geometry-replacing (terminal,
  `UnsupportedRequiredFeature`, ID retained on the display catalog): Slice
  `1001`, Volumetric `1002`, Implicit `1003`, Secure Content `1004`. Any other
  required namespace: `Ignored` record `1000` and continue with Core geometry.
  An undeclared required prefix remains a typed failure.
- `ThreeMfDisplayCatalog` carries a `CompatibilityReportBuilder`; the adapter
  merges it and emits one `CompatibilityReport` chunk through T04. Additional
  records: `1010` per-corner split, `1011` per-corner dominant, `1014`
  translucent display property, `1015` unsupported display property, `1016`
  multiple texture layers, `1017` texture decode fallback, `1018` multiple PBR
  layers.
- Per-corner material rule in `EmitMesh` (`import-worker/src/ThreeMfAdapter.cpp`)
  with pure planner `import-worker/src/ThreeMfMaterialPlan.h`: centroid fan into
  three per-corner sub-triangles when `triangleCount * 3` stays within the
  existing Tier-B triangle ceiling; otherwise (or if a corner-only
  image/material cannot be produced) use the first most-frequent corner.
  Positions, winding, bounds and authored corner UVs are unchanged. Material
  ordering stays first-use deterministic. No new numeric limit.
- Preserved `AllowedType` (`model`, `support`, `solidSupport`, `surface`,
  `other`) and the bounded Beam Lattice fallback unchanged.
- Fixtures/tests: `[3mf-compat]` in `tests/import-isolation/ThreeMfSpikeTests.cpp`
  covers slicer-private metadata namespaces, unknown required namespaces,
  required Slice failure (synthetic and `production-boxes`), the planner's
  split/dominant decisions, a per-corner UV texture package, malformed/truncated
  OPC and the over-long required-prefix limit.
- Docs: `docs/FORMAT-SUPPORT.md`, `docs/design/adapters/3mf-thumbnail.md`,
  new `docs/design/adr/0066-3mf-required-extension-and-per-corner-policy.md`,
  and `docs/0.7.0/progress/T10b.md`.

### Policy table
| Required namespace | Outcome |
| --- | --- |
| Core / Materials / Production / Beam / Ball | continue, no record |
| Slice (`1001`) / Volumetric (`1002`) / Implicit (`1003`) / Secure Content (`1004`) | terminal `UnsupportedRequiredFeature`; feature ID kept |
| anything else | continue with `Ignored` record `1000` |

### Split/degrade rule
All corners equal -> one triangle. Otherwise, if a centroid fan fits the
generation triangle/material ceilings, emit three sub-triangles each with one
corner's grouping and record `1010` (`Degraded`). Otherwise, or when the fan's
material/image cannot be produced, emit one triangle using the first
most-frequent corner and record `1011` (`Degraded`). Never return
`ResourceLimit` while a usable preview remains.

### Fixture licenses
All new fixtures are generated in test code from the repository's own
`core-box.3mf.base64` or built from scratch; the embedded PNG is a public-domain
1x1 image. No third-party slicer asset was committed, so no new license text is
required.

### Deviations / remaining slicer-format omissions
- lib3mf 2.5 binds one property resource to a triangle, so a compliant file
  cannot express differing texture/sampler identities across `p1/p2/p3`. The
  split path is therefore defensive for extended producers; the real fixture
  exercises per-corner UVs and the planner unit test covers the split/degrade
  decision. Recorded as a follow-up in `docs/0.7.0/progress/T10b.md`.
- Plate grouping/print settings, Slice/Secure Content/Volumetric/Implicit/
  toolpath semantics and new geometry types remain out of scope.

### Check results
```
MSBuild Preview3D.slnx /t:Tests_ImportIsolation /p:Configuration=Release /p:Platform=x64  -> pass
x64\Release\Tests.ImportIsolation.exe "[3mf-compat]"                 -> 163 assertions, 5 cases, pass
x64\Release\Tests.ImportIsolation.exe "[3mf-003],[3mf-004],[3mf-005],[3mf-006],[3mf-007],[3mf-spike],[3mf-compat]"
                                                                     -> 2882 assertions, 22 cases, pass
x64\Release\Tests.ImportIsolation.exe "[compatibility],[archive]"    -> 2368 assertions, 17 cases, pass
x64\Release\Tests.ImportIsolation.exe "[hostile-worker],[hostile]"   -> 305 assertions, 16 cases (15 pass, 1 Release fault-harness skip), pass
```


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
- [ ] Split `CheckRequiredExtensions` into:
      - known/understood namespaces (current allowlist);
      - unknown namespaces: record an `Ignored`/`Degraded` compatibility feature
        and continue if core geometry is usable;
      - a required extension that alters visible geometry and has no fallback:
        still `UnsupportedRequiredFeature`, with the feature ID.
- [ ] Per-corner material variation:
      - if a triangle’s corners differ in texture/sampler/texture-layer/mix,
        split the triangle into per-material triangles when doing so stays
        within the triangle/material/chunk budgets;
      - otherwise choose the dominant corner with a `Degraded` warning; do not
        fail the model;
      - keep deterministic material ordering.
- [ ] Preserve the existing allowances for slicer-private object types
      (including `other` referenced by the build) and the bounded Beam Lattice
      fallback.
- [ ] Add compatibility records for ignored required extensions, degraded
      per-corner splits and unsupported-but-not-geometry-critical features.
- [ ] Add fixtures:
      - a PrusaSlicer/Bambu/Cura file with private metadata namespaces but no
        required geometry impact;
      - a model with per-corner texture/sampler variation;
      - a file with a genuinely unsupported required extension that affects
        visible geometry (must fail);
      - malformed/over-budget OPC cases retained.
- [ ] Add `[3mf-compat]` cases asserting outcome, warning feature IDs, split
      triangle/material counts and that malformed/unsafe files still fail.
- [ ] Update `docs/FORMAT-SUPPORT.md` and the 3MF support notes.

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
- [ ] `x64\Release\Tests.ImportIsolation.exe "[3mf-compat]"` passes.
- [ ] The slicer-metadata fixture opens; the per-corner fixture opens with
      split triangles or a documented degraded choice.
- [ ] An unsupported required geometry extension still fails typed.
- [ ] Warning IDs/severities are visible through T04.
- [ ] Archive/OPC limits and hostile-worker cases remain green.
- [ ] Docs updated; no new limit was added without an ADR.
- [ ] Hand-off records the required-extension policy and split/degrade rule.

## Hand-off
_(filled in by the implementing session: policy table, split rule, fixture
licenses, warning IDs and any remaining slicer-format omissions.)_

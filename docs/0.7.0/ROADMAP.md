# Preview 3D 0.7.0 Roadmap — format breadth and rendering quality

Execution plan for 0.7.0, derived from [`../proposal.md`](../proposal.md) and split into
small, independently reviewable sessions. This plan is its own **task set**; the base/MVP
and `security` sets are untouched.

```powershell
./.symphony/symphony.ps1 doctor --set 0.7.0
./.symphony/symphony.ps1 run    --set 0.7.0
./.symphony/symphony.ps1 status --set 0.7.0
```

Task files live in [`../tasks/0.7.0/`](../tasks/0.7.0/). Read
[`../tasks/0.7.0/README.md`](../tasks/0.7.0/README.md) first for the ground rules, phase map
and per-task verify commands. Each task carries its own `verify:` front matter.

T01–T19 (including T10b) completed the original plan. **T20–T35 plus T21a are the follow-up work**
post-qualification work for Studio, Clay and Directional. Read the
[render-quality findings and shared acceptance contract](RENDER-QUALITY-FOLLOWUP.md)
before starting T20. These additions preserve the completed tasks and run in
the order below. T20 creates the new verification runner used by later tasks;
the new commands are planned interfaces, not previously passing checks.

Tasks run in roadmap order. T02 and T04 are foundations; dependent tasks must wait for them.
T01 may run in parallel with T02. T14/T15/T16 can run in parallel with the import-side
breadth tasks once T02 is stable.

## Phase 0 — Baseline

- [x] T01 — Compatibility corpus and baseline scoreboard → [../tasks/0.7.0/T01-compatibility-corpus.md](../tasks/0.7.0/T01-compatibility-corpus.md)

## Phase 1 — Foundation

- [x] T02 — Format catalog and content-aware router → [../tasks/0.7.0/T02-format-catalog-router.md](../tasks/0.7.0/T02-format-catalog-router.md)
- [x] T03 — Catalog-driven registration, filters and Open As → [../tasks/0.7.0/T03-registration-filters-open-as.md](../tasks/0.7.0/T03-registration-filters-open-as.md)
- [x] T04 — Compatibility severity model and report chunk → [../tasks/0.7.0/T04-compatibility-report.md](../tasks/0.7.0/T04-compatibility-report.md)

## Phase 2 — Import breadth and presentation

T06 depends only on T02 and is sequenced at the start of this phase so task ids stay ascending.

- [x] T05 — glTF required-extension compatibility → [../tasks/0.7.0/T05-gltf-extension-compatibility.md](../tasks/0.7.0/T05-gltf-extension-compatibility.md)
- [x] T06 — Per-format presentation defaults and orientation → [../tasks/0.7.0/T06-presentation-defaults-orientation.md](../tasks/0.7.0/T06-presentation-defaults-orientation.md)
- [x] T07 — STEP tolerance and import quality → [../tasks/0.7.0/T07-step-tolerance-import-quality.md](../tasks/0.7.0/T07-step-tolerance-import-quality.md)
- [x] T08 — OCCT host: IGES and BREP → [../tasks/0.7.0/T08-cad-host-iges-brep.md](../tasks/0.7.0/T08-cad-host-iges-brep.md)
- [x] T09 — OCCT host: XBF and FCStd → [../tasks/0.7.0/T09-cad-host-xbf-fcstd.md](../tasks/0.7.0/T09-cad-host-xbf-fcstd.md)
- [x] T10 — Bounded Assimp compatibility adapter → [../tasks/0.7.0/T10-assimp-compatibility-adapter.md](../tasks/0.7.0/T10-assimp-compatibility-adapter.md)
- [x] T10b — 3MF best-effort compatibility → [../tasks/0.7.0/T10b-three-mf-compatibility.md](../tasks/0.7.0/T10b-three-mf-compatibility.md)
- [x] T11 — USD primitives, points and crease tolerance → [../tasks/0.7.0/T11-usd-primitives-points-creases.md](../tasks/0.7.0/T11-usd-primitives-points-creases.md)

## Phase 3 — Studio lighting and presentation

- [x] T12 — TGA/DDS textures and ORM ambient occlusion → [../tasks/0.7.0/T12-textures-tga-dds-ao.md](../tasks/0.7.0/T12-textures-tga-dds-ao.md)
- [x] T13 — HDR/EXR textures and environment assets → [../tasks/0.7.0/T13-textures-hdr-exr-environment.md](../tasks/0.7.0/T13-textures-hdr-exr-environment.md)
- [x] T14 — Studio IBL and neutral tone map (first slice) → [../tasks/0.7.0/T14-studio-ibl-neutral-tonemap.md](../tasks/0.7.0/T14-studio-ibl-neutral-tonemap.md)
- [x] T15 — HDR target, exposure, background and presets → [../tasks/0.7.0/T15-hdr-target-exposure-presets.md](../tasks/0.7.0/T15-hdr-target-exposure-presets.md)
- [x] T16 — Ground contact shadow and ambient occlusion → [../tasks/0.7.0/T16-ground-shadow-ao.md](../tasks/0.7.0/T16-ground-shadow-ao.md)

## Phase 4 — Render depth

- [x] T17 — Line/strip/fan/point topology → [../tasks/0.7.0/T17-line-strip-fan-point-topology.md](../tasks/0.7.0/T17-line-strip-fan-point-topology.md)
- [x] T18 — Material lobes and transparency → [../tasks/0.7.0/T18-material-lobes-transparency.md](../tasks/0.7.0/T18-material-lobes-transparency.md)

## Phase 5 — Qualify

- [x] T19 — 0.7.0 compatibility qualification and docs → [../tasks/0.7.0/T19-qualification-docs.md](../tasks/0.7.0/T19-qualification-docs.md)

## Phase 6 — Reproducible visual evidence

- [x] T20 — Deterministic visual comparison harness and observed baseline → [../tasks/0.7.0/T20-visual-comparison-harness.md](../tasks/0.7.0/T20-visual-comparison-harness.md)
- [x] T21 — Material and light-contribution diagnostics → [../tasks/0.7.0/T21-render-diagnostics.md](../tasks/0.7.0/T21-render-diagnostics.md)

- [ ] T21a — Windows Photos-style background and ground-highlight settings → [../tasks/0.7.0/T21a-native-background-settings.md](../tasks/0.7.0/T21a-native-background-settings.md)

## Phase 7 — Authored inputs and bounded complete loads

- [~] T22 — Preserve authored material, UV, normal and AO semantics → [../tasks/0.7.0/T22-material-semantics.md](../tasks/0.7.0/T22-material-semantics.md) ⟵ running
- [ ] T23 — Texture-budget accounting and complete-load readiness → [../tasks/0.7.0/T23-texture-budgets-load-readiness.md](../tasks/0.7.0/T23-texture-budgets-load-readiness.md)

## Phase 8 — Correct shading and textured lobes

- [ ] T24 — Correct BRDF, environment integration and clearcoat layering → [../tasks/0.7.0/T24-brdf-ibl-layering.md](../tasks/0.7.0/T24-brdf-ibl-layering.md)
- [ ] T25 — Texture-driven clearcoat, specular, sheen and transmission inputs → [../tasks/0.7.0/T25-textured-material-lobes.md](../tasks/0.7.0/T25-textured-material-lobes.md)

## Phase 9 — Layered glass and transmission

- [ ] T26 — Bounded order-independent transparency → [../tasks/0.7.0/T26-layered-transparency.md](../tasks/0.7.0/T26-layered-transparency.md)
- [ ] T27 — Scene-based transmission and tinted or rough glass → [../tasks/0.7.0/T27-scene-transmission.md](../tasks/0.7.0/T27-scene-transmission.md)

## Phase 10 — Studio, Clay and Directional quality

- [ ] T28 — Calibrate Studio environment and first-open presentation → [../tasks/0.7.0/T28-studio-lighting-quality.md](../tasks/0.7.0/T28-studio-lighting-quality.md)
- [ ] T29 — Clay as a coherent opaque geometry-review mode → [../tasks/0.7.0/T29-clay-lighting-quality.md](../tasks/0.7.0/T29-clay-lighting-quality.md)
- [ ] T30 — Directional lighting, reflection and sun consistency → [../tasks/0.7.0/T30-directional-lighting-quality.md](../tasks/0.7.0/T30-directional-lighting-quality.md)

## Phase 11 — Contact, depth and sampling

- [ ] T31 — Geometry-derived ground and contact shadows → [../tasks/0.7.0/T31-geometry-ground-shadows.md](../tasks/0.7.0/T31-geometry-ground-shadows.md)
- [ ] T32 — Scale-aware screen-space ambient occlusion → [../tasks/0.7.0/T32-screen-space-ambient-occlusion.md](../tasks/0.7.0/T32-screen-space-ambient-occlusion.md)
- [ ] T33 — Edge, specular and texture sampling quality → [../tasks/0.7.0/T33-antialiasing-texture-filtering.md](../tasks/0.7.0/T33-antialiasing-texture-filtering.md)

## Phase 12 — Independent corpus and release qualification

- [ ] T34 — Complete corpus, cross-format and hostile-input render qualification → [../tasks/0.7.0/T34-full-corpus-render-regressions.md](../tasks/0.7.0/T34-full-corpus-render-regressions.md)
- [ ] T35 — Independent visual release gate and final 0.7.0 evidence → [../tasks/0.7.0/T35-render-quality-release-gate.md](../tasks/0.7.0/T35-render-quality-release-gate.md)

<!-- symphony:status -->
**Pipeline status** — updated 2026-10-30T19:21:15Z · 22/36 done

- Completed: T01, T02, T03, T04, T05, T06, T07, T08, T09, T10, T10b, T11, T12, T13, T14, T15, T16, T17, T18, T19, T20, T21
- Blocked: none
- Failed: none
- Remaining: T22, T23, T24, T25, T26, T27, T28, T29, T30, T31, T32, T33, T34, T35
- Last finished: T21 — done · T21 diagnostic channels, numeric HDR readback, per-material draw records, probe fixture/profile, Release rejection and cause-by-region evidence all landed and pass the required verification.
<!-- /symphony:status -->

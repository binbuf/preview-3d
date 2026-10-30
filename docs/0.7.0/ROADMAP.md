# Preview 3D 0.7.0 Roadmap — format breadth and Studio lighting

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
- [ ] T10b — 3MF best-effort compatibility → [../tasks/0.7.0/T10b-three-mf-compatibility.md](../tasks/0.7.0/T10b-three-mf-compatibility.md)
- [ ] T11 — USD primitives, points and crease tolerance → [../tasks/0.7.0/T11-usd-primitives-points-creases.md](../tasks/0.7.0/T11-usd-primitives-points-creases.md)

## Phase 3 — Studio lighting and presentation

- [ ] T12 — TGA/DDS textures and ORM ambient occlusion → [../tasks/0.7.0/T12-textures-tga-dds-ao.md](../tasks/0.7.0/T12-textures-tga-dds-ao.md)
- [ ] T13 — HDR/EXR textures and environment assets → [../tasks/0.7.0/T13-textures-hdr-exr-environment.md](../tasks/0.7.0/T13-textures-hdr-exr-environment.md)
- [ ] T14 — Studio IBL and neutral tone map (first slice) → [../tasks/0.7.0/T14-studio-ibl-neutral-tonemap.md](../tasks/0.7.0/T14-studio-ibl-neutral-tonemap.md)
- [ ] T15 — HDR target, exposure, background and presets → [../tasks/0.7.0/T15-hdr-target-exposure-presets.md](../tasks/0.7.0/T15-hdr-target-exposure-presets.md)
- [ ] T16 — Ground contact shadow and ambient occlusion → [../tasks/0.7.0/T16-ground-shadow-ao.md](../tasks/0.7.0/T16-ground-shadow-ao.md)

## Phase 4 — Render depth

- [ ] T17 — Line/strip/fan/point topology → [../tasks/0.7.0/T17-line-strip-fan-point-topology.md](../tasks/0.7.0/T17-line-strip-fan-point-topology.md)
- [ ] T18 — Material lobes and transparency → [../tasks/0.7.0/T18-material-lobes-transparency.md](../tasks/0.7.0/T18-material-lobes-transparency.md)

## Phase 5 — Qualify

- [ ] T19 — 0.7.0 compatibility qualification and docs → [../tasks/0.7.0/T19-qualification-docs.md](../tasks/0.7.0/T19-qualification-docs.md)

<!-- symphony:status -->
**Pipeline status** — updated 2026-10-30T06:12:00Z · 10/20 done

- Completed: T01, T02, T03, T04, T05, T06, T07, T08, T09, T10
- Blocked: none
- Failed: none
- Remaining: T10b, T11, T12, T13, T14, T15, T16, T17, T18, T19
- Last finished: T10 — done · Bounded Assimp compatibility adapter landed for DAE/3DS/DXF/X/OFF/AMF/X3D with broker IOSystem, caps, fixtures, tests, fuzz lane, catalog/installer/docs, overlay X3D fix, ADR-0065, and passing acceptance checks.
<!-- /symphony:status -->

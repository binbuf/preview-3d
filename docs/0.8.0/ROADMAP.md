# Preview 3D 0.8.0 Roadmap — valid-file coverage

Execution plan for 0.8.0: make the formats Preview 3D already claims to support
open the valid files users actually have, not only the happy-path subset each
importer was built against. This plan is its own **task set**; the base/MVP,
`security`, and `0.7.0` sets are untouched.

```powershell
./.symphony/symphony.ps1 doctor --set 0.8.0
./.symphony/symphony.ps1 run    --set 0.8.0
./.symphony/symphony.ps1 status --set 0.8.0
```

Task files live in [`../tasks/0.8.0/`](../tasks/0.8.0/). Read
[`../tasks/0.8.0/README.md`](../tasks/0.8.0/README.md) first for the ground
rules, phase map and per-task verify commands. Each task carries its own
`verify:` front matter.

The work is ordered so the scoreboard lands first, then extension/detection
completeness, then primitive completeness, then options and reader selection,
then real-world evidence and qualification. T02/T03 and T04/T05 may run in
parallel after T01.

## Phase 0 — Baseline

- [x] T01 - Valid-file coverage baseline and failure triage → [../tasks/0.8.0/T01-valid-file-coverage-baseline.md](../tasks/0.8.0/T01-valid-file-coverage-baseline.md)

## Phase 1 — Extension and detection completeness

- [x] T02 - Extension aliases and catalog/installer drift → [../tasks/0.8.0/T02-extension-aliases-registration.md](../tasks/0.8.0/T02-extension-aliases-registration.md)
- [x] T03 - Catalog-driven content sniffer → [../tasks/0.8.0/T03-catalog-driven-sniffer.md](../tasks/0.8.0/T03-catalog-driven-sniffer.md)

## Phase 2 — Primitive completeness

- [x] T04 - Assimp point and line primitives → [../tasks/0.8.0/T04-assimp-point-line-primitives.md](../tasks/0.8.0/T04-assimp-point-line-primitives.md)
- [ ] T05 - CAD edge and wire geometry → [../tasks/0.8.0/T05-cad-edge-wire-geometry.md](../tasks/0.8.0/T05-cad-edge-wire-geometry.md)

## Phase 3 — Options and reader selection

- [ ] T06 - Per-format import options and quality surface → [../tasks/0.8.0/T06-per-format-import-options.md](../tasks/0.8.0/T06-per-format-import-options.md)
- [ ] T07 - Reader registry, scores and force-reader → [../tasks/0.8.0/T07-reader-registry-force-reader.md](../tasks/0.8.0/T07-reader-registry-force-reader.md)
- [ ] T08 - Compatibility readers and bounded fallback → [../tasks/0.8.0/T08-compatibility-readers-fallback.md](../tasks/0.8.0/T08-compatibility-readers-fallback.md)

## Phase 4 — Evidence and qualification

- [ ] T09 - Real-world per-format corpus and coverage report → [../tasks/0.8.0/T09-real-world-coverage-corpus.md](../tasks/0.8.0/T09-real-world-coverage-corpus.md)
- [ ] T10 - Support matrix, docs and 0.8.0 qualification → [../tasks/0.8.0/T10-support-matrix-qualification.md](../tasks/0.8.0/T10-support-matrix-qualification.md)

## Deferred to a later set

- Tier B progressive/streaming for large valid OBJ/FBX/3MF/USD/CAD files.
- Thumbnail-provider expansion beyond the frozen eight families (ADR-0001);
  this needs a new ADR and provider adapters, not just registration.
- New primary formats such as VRML, IFC, LAS/LAZ, Alembic, or VDB.
- Animation playback, cameras and lights.

<!-- symphony:status -->
**Pipeline status** — updated 2026-11-07T16:53:30Z · 4/10 done

- Completed: T01, T02, T03, T04
- Blocked: none
- Failed: none
- Remaining: T05, T06, T07, T08, T09, T10
- Last finished: T04 — done · Assimp point/line/triangle emission, mixed chunks, topology-aware budgets, fixtures/tests, valid-file flips, docs/ADR-0098; Release and Debug gates pass.
<!-- /symphony:status -->

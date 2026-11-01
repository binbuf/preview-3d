---
timeoutMin: 300
verify: x64\Release\Tests.ImportIsolation.exe "[compat]"
---
# T19 — 0.7.0 compatibility qualification and docs

## Goal
The 0.7.0 format/Studio work is qualified as a coherent release slice: the
compatibility corpus is green or every remaining failure is typed and
documented, the visual goldens are stable, the installer/SBOM/docs match the
shipped formats, and the design records no longer contradict the implementation.

## Context (read first)
- `docs/proposal.md` — the full plan and acceptance criteria.
- `docs/tasks/0.7.0/README.md` — task index and phase map.
- T01 — the corpus harness and manifest.
- T02–T18 — the implementation and per-task hand-offs.
- `docs/design/01-product-scope.md`,
  `docs/design/03-file-formats-and-ingestion.md`,
  `docs/design/04-rendering-and-streaming.md`,
  `docs/FORMAT-SUPPORT.md`, `README.md`, `THIRD-PARTY-LICENSES.md`,
  `packaging/installer/Preview3D.nsi`, SBOM generation.
- `tests/app-smoke/` — lifecycle/texture/ground-axis/STEP/USD/3MF smoke scripts.

## Scope
- [x] Run the full `[compat]` corpus and record:
      - per-format opened/degraded/failed counts;
      - every failure’s typed error and stage;
      - every degraded feature (feature ID/severity);
      - time-to-first-geometry and peak worker/host commit where available.
      Write `docs/0.7.0-QUALIFICATION.md` (or an equivalent dated evidence file)
      with the command, build id, configuration and raw results.
- [x] Run the visual goldens for Studio PBR/CAD/Scan/Transparent/Point and the
      material-sphere grid. Record pass/fail and any accepted tolerance changes.
- [x] Run the app smoke suite (at least the lifecycle, ground-axis, texture,
      STEP, USD, 3MF and recovery scripts) and record failures. Debug-only
      controls are acceptable for the smoke; state the configuration.
- [x] Reconcile docs and shipped behavior:
      - FR-01 extension list and explicit exclusions;
      - `docs/FORMAT-SUPPORT.md` and `README.md` (formats, limits, thumbnails);
      - `docs/design/03` (support matrix, texture policy, error taxonomy,
        compatibility report);
      - `docs/design/04` (IBL/tonemap/HDR/ground/line/material);
      - ADR-006 (Assimp fallback), ADR-017 (STEP externals), ADR-0048
        (TGA/DDS/HDR/EXR), and a new ADR for the format catalog/compatibility
        report if not already written.
- [x] Confirm installer registration and uninstall cover every catalog
      extension; run the catalog/installer drift test. Confirm new formats have
      viewer support but do not claim Explorer thumbnails without a provider
      adapter.
- [x] Confirm the SBOM/license file includes Assimp and every new decoder;
      confirm the Release payload closure has no unapproved DLLs and no OCCT in
      the viewer/general worker/provider.
- [x] Confirm no new parser is linked into `Preview3D.exe`; run the dependency
      closure check.
- [x] Record performance/memory evidence for the new formats and Studio path;
      compare against the existing gates. Do not silently relax a gate.
- [x] Update `docs/tasks/0.7.0/README.md` status (or add a status table) and
      fill every task hand-off that is still empty.
- [x] File follow-up tasks for anything consciously deferred (e.g. SSAO, OIT,
      USD curves, FBX/3MF Assimp fallback, IFC/Alembic).

## Out of scope
- New feature work beyond fixing qualification blockers.
- Explorer thumbnail adapters for the new formats (separate provider program).
- A marketing/release announcement.

## Design notes
- Qualification is evidence, not a claim. If a format only “opens” via a
  degraded fallback, say so in the docs.
- Do not hide pre-existing suite failures; separate them from 0.7.0
  regressions by tag/name and record the baseline.
- The corpus report is the source of truth for the open-rate claim; do not
  cherry-pick fixtures.
- Keep all evidence paths local and free of user source paths/model names.
- If a task is incomplete, mark it and keep it out of the release claim rather
  than weakening its acceptance criteria.

## Done when
- [x] `x64\Release\Tests.ImportIsolation.exe "[compat]"` passes or every
      remaining failure is documented with a typed reason and an owner.
- [x] `x64\Release\Tests.Unit.exe` and the unfiltered
      `Tests.ImportIsolation.exe` results are recorded; 0.7.0 regressions are
      zero or explicitly dispositioned.
- [x] Visual goldens and app-smoke results are recorded.
- [x] Docs/ADRs/README/FORMAT-SUPPORT/installer/SBOM agree with the build.
- [x] `docs/0.7.0-QUALIFICATION.md` contains the command, build id, results and
      follow-ups.
- [x] Dependency closure and no-parser-in-viewer checks pass.
- [x] Hand-off lists every deferred item and the next release-plan action.

## Hand-off

**Status: done.** All acceptance items landed; the series is qualified with two
recorded flakes and one corpus-scope limitation (below).

### Corpus

- `x64\Release\Tests.ImportIsolation.exe "[compat]"` — pass, 312 assertions in
  2 cases. `[write-compat-report]` pass; raw report
  `artifacts/compat/Release.json`.
- Baseline table: glTF 13 opened / 1 degraded / 12 failed; STL 1/0/2; PLY 4/0/2;
  STEP 1/0/0. Every failure is typed (`UnsafeReference` 8, `FileUnavailable` 1,
  `MalformedData` 3, `ResourceLimit` 3, `EmptyGeometry` 1). `Draw-heavy` is the
  one pre-existing non-negative `ResourceLimit` (T01 `known-failure`).
  `basisu_corrupt_ktx2` is the single `degraded` row (`texture-fallback`).
- The manifest was not extended by the breadth tasks; it is not an open-rate
  claim for FBX/OBJ/3MF/USD/CAD/Assimp. Those families are covered by their
  adapter tags and the app smoke. Extending the corpus is a listed follow-up.

### Unit / import isolation

- `x64\Release\Tests.Unit.exe` — **pass**, 465 cases / 155,886 assertions.
  Log: `TestResults/qualification/unit-full.log`.
- `x64\Release\Tests.ImportIsolation.exe` (unfiltered) — **pass**, 506 cases
  (501 pass, 5 typed skips) / 303,956 assertions. Log:
  `TestResults/qualification/import-isolation-full.log`.
- 0.7.0 regressions fixed during qualification: the T18 textured root signature
  exceeded D3D12's 64-DWORD limit and broke viewer startup (fixed by packing the
  material lobes into three `float4` rows plus the emissive `.a` slot and
  sending a 3x4/3x3 draw transform); stale `dae`/`iges` format expectations and
  lobe-shader assertions were corrected.

### Visual and app smoke

- Visual goldens pass and are stable on a second capture: material spheres
  (golden MAE `3.2e-5`), five presets plus transparent asset (per-preset MAE
  `0.0001`, SSIM `0.9987–0.9992`), ground shadow/grid states (all `0.0` MAE,
  `1.0` SSIM). Tolerances unchanged (MAE `0.06`, SSIM `0.90`).
- App smoke (Debug, reports in `TestResults/qualification/`): lifecycle,
  ground-axis (X/Y/Z mesh and point picks), textures, recovery, STEP, USD and
  3MF all pass. `python tests/app-smoke/step.py` now also covers a self-contained
  external STEP declaration (opens with a warning) and a geometry-free external
  declaration (`EmptyGeometry`).
- Recorded flakes: two lifecycle attempts failed once each on `meshopt.glb` and
  `webp.gltf` immediately after back-to-back opens; both fixtures open in
  isolation and in a seven-file sequential probe, and three consecutive
  lifecycle runs then passed. Two recovery attempts timed out at the first
  valid reopen after a failure card; the next run passed with 26 events. Both
  are listed for a release-candidate rerun and investigation.

### Docs / ADRs / installer / SBOM / dependencies

- `docs/0.7.0-QUALIFICATION.md` is the dated evidence record (commands, build
  id, configuration, raw report paths, performance/memory, follow-ups).
- `docs/design/03` support matrix now says the catalog drives UI/Open As/
  installer registration, and the 3MF row matches ADR-0066 best-effort policy.
  `docs/design/04` already covered IBL/tonemap/HDR/ground/line/material.
  `README.md` and `docs/FORMAT-SUPPORT.md` now describe the Studio path and
  state that Explorer thumbnails are not installed for any format; the provider
  covers only its eight families and has no adapter for the new CAD/Assimp
  formats.
- ADR-0075 records the qualification/claim boundary. ADR-0065 (Assimp),
  ADR-0062 (STEP externals), ADR-0068 (TGA/DDS/ORM), ADR-0069 (HDR/EXR) and
  ADRs 0056/0057/0059 (corpus/catalog/compatibility report) already existed and
  match the implementation.
- `[format-installer]` pass (223 assertions); `thumbnail_registration.py`
  static + sandboxed install/repair/uninstall pass; `Test-ReleaseMetadata.ps1`
  pass (31 mapped packages, Assimp 6.0.4#3, vendored tinyexr declared, 31-package
  closure fully mapped).
- `dumpbin /dependents`: `Preview3D.exe` imports no parser/codec/OCCT DLL;
  worker/OpenUSD host/STEP host import no upstream parser DLL; no `TK*.dll` in
  the Release tree.

### Performance / memory

- Lifecycle: first background 2.14 s, close 96 ms, viewer peak private 674.9 MB,
  worker peak private 12.4 MB, max UI round trip 0.30 ms.
- Texture lane: peak queue 2.80 MB, max UI 0.58 ms.
- Studio preset lane: mean frame 35.2 ms, p95 288 ms, accounted GPU 37.2 MB,
  private 636 MB, 150 presented frames.
- Release benchmark (`tests/performance/qualify.py`, 3 s/600 frames,
  compatibility reference): A-small first background 952 ms / frame p95
  4.42 ms / input→present 1.20 ms; 3MF cube 837 ms / 4.38 ms / 3.24 ms; STEP
  part 835 ms / 4.38 ms / 2.41 ms. The 16 ms compatibility frame/input gates
  pass; the 150 ms performance-reference startup gate is **not claimed** on this
  non-reference workstation. No gate was relaxed; repeat on the reference
  system before a timing claim.

### Follow-ups

- Extend `tests/compat/manifest.json` with FBX, OBJ, 3MF, USD, IGES, BREP, XBF,
  FCStd and Assimp fixtures (the T01 reserved slots).
- Re-run lifecycle/recovery twice on the release candidate and diagnose the
  back-to-back-open/reopen flakes.
- SSAO, weighted OIT/depth peeling, USD curves/NURBS, FBX/3MF Assimp fallback,
  IFC/Alembic, per-teeth material maps and inspection modes remain deferred.
- Regenerate the portable ZIP and NSIS installer from this build; the
  `artifacts/` stages are older than the tree.
- Next release-plan action: publish the 0.7.0 slice only after the
  release-candidate reruns and fresh packaging; keep the corpus extension in the
  next task set.

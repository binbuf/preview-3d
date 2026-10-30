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
- [ ] Run the full `[compat]` corpus and record:
      - per-format opened/degraded/failed counts;
      - every failure’s typed error and stage;
      - every degraded feature (feature ID/severity);
      - time-to-first-geometry and peak worker/host commit where available.
      Write `docs/0.7.0-QUALIFICATION.md` (or an equivalent dated evidence file)
      with the command, build id, configuration and raw results.
- [ ] Run the visual goldens for Studio PBR/CAD/Scan/Transparent/Point and the
      material-sphere grid. Record pass/fail and any accepted tolerance changes.
- [ ] Run the app smoke suite (at least the lifecycle, ground-axis, texture,
      STEP, USD, 3MF and recovery scripts) and record failures. Debug-only
      controls are acceptable for the smoke; state the configuration.
- [ ] Reconcile docs and shipped behavior:
      - FR-01 extension list and explicit exclusions;
      - `docs/FORMAT-SUPPORT.md` and `README.md` (formats, limits, thumbnails);
      - `docs/design/03` (support matrix, texture policy, error taxonomy,
        compatibility report);
      - `docs/design/04` (IBL/tonemap/HDR/ground/line/material);
      - ADR-006 (Assimp fallback), ADR-017 (STEP externals), ADR-0048
        (TGA/DDS/HDR/EXR), and a new ADR for the format catalog/compatibility
        report if not already written.
- [ ] Confirm installer registration and uninstall cover every catalog
      extension; run the catalog/installer drift test. Confirm new formats have
      viewer support but do not claim Explorer thumbnails without a provider
      adapter.
- [ ] Confirm the SBOM/license file includes Assimp and every new decoder;
      confirm the Release payload closure has no unapproved DLLs and no OCCT in
      the viewer/general worker/provider.
- [ ] Confirm no new parser is linked into `Preview3D.exe`; run the dependency
      closure check.
- [ ] Record performance/memory evidence for the new formats and Studio path;
      compare against the existing gates. Do not silently relax a gate.
- [ ] Update `docs/tasks/0.7.0/README.md` status (or add a status table) and
      fill every task hand-off that is still empty.
- [ ] File follow-up tasks for anything consciously deferred (e.g. SSAO, OIT,
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
- [ ] `x64\Release\Tests.ImportIsolation.exe "[compat]"` passes or every
      remaining failure is documented with a typed reason and an owner.
- [ ] `x64\Release\Tests.Unit.exe` and the unfiltered
      `Tests.ImportIsolation.exe` results are recorded; 0.7.0 regressions are
      zero or explicitly dispositioned.
- [ ] Visual goldens and app-smoke results are recorded.
- [ ] Docs/ADRs/README/FORMAT-SUPPORT/installer/SBOM agree with the build.
- [ ] `docs/0.7.0-QUALIFICATION.md` contains the command, build id, results and
      follow-ups.
- [ ] Dependency closure and no-parser-in-viewer checks pass.
- [ ] Hand-off lists every deferred item and the next release-plan action.

## Hand-off
_(filled in by the implementing session: corpus table, visual/app-smoke results,
docs/ADR updates, installer/SBOM evidence, regressions, and follow-up tasks.)_

---
verify: pwsh -NoProfile -File .symphony/symphony.ps1 lint
---
# T01 — Freeze the provider specification and eight-family roster

## Goal
Remove every blocking contradiction from the provider specification so adapter work can be scheduled
against one authority: eight family CLSIDs including STEP, an explicit OCCT linkage decision, an
explicit provider decoder scope, and a chosen registration vehicle.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — the provider contract; its CLSID table now lists eight families including STEP and must stay frozen.
- `docs/design/overview.md` — the program map and the eight-family target roster.
- `docs/design/adr/0001`–`0006` — the six decisions this task must accept and reconcile.
- `docs/design/11-decisions-and-risks.md` — ADR-011, R-08, R-15, R-21 and Spike 8.
- `docs/design/02-system-architecture.md` — the "OCCT is never the thumbnail provider" wording ADR-0002 supersedes.
- `docs/design/08-installation-and-registration.md` — COM registration identities and rules.
- `docs/design/adapters/step-009-thumbnail.md` — the STEP adapter requirement for a new CLSID.

## Scope
- [ ] Confirm and freeze the STEP CLSID `{6EE961AC-AC3B-4958-A898-E30523FEE79D}` already recorded in `05-thumbnail-provider.md` and `overview.md`; do not regenerate it. (`08` has no CLSID table of its own; its registrations reference `05`.)
- [ ] Confirm no normative "seven CLSIDs"/"seven stable COM classes" statement remains in `docs/design/` (eight families, covering `.step`/`.stp`); the historical conflict descriptions may keep the quoted baseline wording.
- [ ] Confirm ADR-0001 through ADR-0007 are `accepted` and make the baseline docs (`01`, `05`, `08`, `11`, `overview`) agree with them.
- [ ] Reconcile `02-system-architecture.md` with ADR-0002 (OCCT only via a separate bounded adapter linked into the DLL) and `05-thumbnail-provider.md`/`11-decisions-and-risks.md` with ADR-0003 (decoder scope).
- [ ] Record the provider limit/HRESULT source of truth (the thumbnail column of `03-file-formats-and-ingestion.md`) that T06 will encode; distinguish hard accountable caps from the measured 384 MiB process target and cooperative 2 s stop point.
- [ ] State the registration vehicle chosen by ADR-0006 and ADR-0007 (per-machine NSIS now, same identities adopted by the eventual MSI). Mark the extension-level ShellEx location provisional until T03 tests another default app and a per-user association.

## Out of scope
- Writing provider code or interfaces (→ T04, T11–T17).
- Family viewer release qualification (FBX-007, 3MF-007, USD-009, STEP-008) — owned by the viewer program.
- Installer implementation (→ T41).

## Design notes
- The seven existing CLSIDs are product identities and MUST NOT be regenerated or renamed.
- Prefer editing the copied docs in `docs/design/`; the archived `docs/legacy/` copies are historical and stay untouched.
- Keep the STEP CLSID in the same canonical `{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}` form; it is already recorded in both `overview.md` and `05-thumbnail-provider.md` and must not change.

## Done when
- [ ] `pwsh -NoProfile -File .symphony/symphony.ps1 lint` reports no errors.
- [ ] No normative "seven CLSID"/"seven stable" statement remains in `docs/design/`; the CLSID table has eight family rows. The historical conflict descriptions in `overview.md` and ADR-0001 may keep the quoted baseline wording.
- [ ] ADR-0001–0007 are `accepted` and `overview.md` links the STEP identity.
- [ ] Docs touched: `design/05-thumbnail-provider.md`, `design/08-installation-and-registration.md`, `design/02-system-architecture.md`, `design/11-decisions-and-risks.md`, `design/overview.md`, `design/adr/*`.
- [ ] Hand-off below filled in.

## Hand-off

### What landed
- Froze the eight-family roster and the STEP identity `{6EE961AC-AC3B-4958-A898-E30523FEE79D}` in
  `design/05-thumbnail-provider.md` (new "Frozen specification" section) and `design/overview.md`
  (now links ADR-0001 and `adapters/step-009-thumbnail.md`). The seven earlier CLSIDs are unchanged.
- Recorded the provider limit/HRESULT source of truth in `05`: the *Thumbnail host* column of
  `03-file-formats-and-ingestion.md` plus the `05` HRESULT table, as the authority T06 encodes.
  Distinguishes hard accountable caps (256 MiB stream, 128 MiB backing, 192 MiB scratch, per-component
  caps, 384 MiB product-owned ledger) from the measured 384 MiB process-commit target and the
  cooperative 2 s stop point.
- Reconcilements: `02-system-architecture.md` now states the ADR-0002 supersession of the archived
  "OCCT is never the thumbnail provider" wording; `11-decisions-and-risks.md` TSK-209 no longer
  contradicts ADR-0003 (its "does not link either decoder" sentence is retained only as a quoted
  historical baseline); `08-installation-and-registration.md` records the T01 freeze and the
  provisional extension-level `ShellEx` location pending T03.
- Verified ADR-0001 through ADR-0007 are all `accepted`; no ADR content needed changing.

### Deviations
- The reviewed `design/` copies already contained most of the reconciliation when this run started
  (the design package was seeded already agreeing with the ADRs). Work was therefore confirmation
  plus the residual edits above, not a rewrite. No new ADR was created: the decisions were already
  captured by ADR-0001/0002/0003/0006/0007.

### Check results
- `pwsh -NoProfile -File .symphony/symphony.ps1 lint` → `lint: ok (29 tasks)`.
- `Select-String` sweep: no normative "seven CLSID"/"seven stable" statement remains; the only
  remaining "seven" hits are historical quotes in `overview.md`, `adr/0001` and `adr/0007`. CLSID
  table has eight family rows in both `05` and `overview`.

### Changed docs
- `docs/design/05-thumbnail-provider.md`, `docs/design/overview.md`,
  `docs/design/02-system-architecture.md`, `docs/design/08-installation-and-registration.md`,
  `docs/design/11-decisions-and-risks.md`.
- `docs/design/adr/0001`–`0007` verified unchanged (all `accepted`).

### Next task must know
- The provider contract is frozen: eight CLSIDs, in-DLL constrained OCCT adapter (ADR-0002), decoder
  scope (ADR-0003), NSIS-now/MSI-later registration (ADR-0006/0007). Do not regenerate any CLSID.
- T06's limit source of truth is the `03` Thumbnail host column + the `05` HRESULT table.
- No blockers. T02/T03 can proceed.

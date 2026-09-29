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
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_

# T41 — Register all eight CLSIDs and ShellEx handlers

## Goal
Install the provider so Explorer actually uses it: machine-level COM class registration plus each
extension's thumbnail-handler `ShellEx` entry, with non-clobber conflict, repair and uninstall
behavior — without touching the user's default apps.

## Context (read first)
- `docs/design/08-installation-and-registration.md` — "Thumbnail COM registration" and the exact key shape.
- `docs/design/adr/0007-provider-program-scope-and-installer.md` (per-machine NSIS is the current vehicle; MSI later)
- `docs/design/adr/0006-registration-through-installer.md` — the chosen vehicle and rules.
- `docs/design/adr/0001-eight-family-clsid-roster.md` — the eight identities, including STEP.
- `packaging/installer/Preview3D.nsi`, `packaging/installer/Create-Installer.ps1` — the current NSIS path.
- `docs/tasks/11-com-core-lifetime.md` — the DLL's COM exports (`DllGetClassObject`/`DllCanUnloadNow` only; no self-registration).

## Scope
- [ ] Write `HKLM\Software\Classes\CLSID\{family}\InprocServer32` with `(Default)` = the installed DLL path and `ThreadingModel = Apartment` for all eight CLSIDs.
- [ ] Implement the ShellEx mapping location validated by T03 for all direct extensions and verify effective routing with a third-party default ProgID and per-user association; add no `IPreviewHandler`, context-menu, property, icon-overlay or property-handler entry.
- [ ] Implement non-clobber: if a pre-existing non-product handler occupies a value, record a conflict and preserve it. Define NSIS repair as an idempotent same-version installer rerun, since the current ARP entry sets `NoRepair=1`; rerun restores missing product files/keys without overwriting another handler.
- [ ] Make uninstall remove only product-owned keys/values and broadcast `SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, null, null)` from a non-elevated notification action.
- [ ] Add install/conflict/same-version-rerun/uninstall tests, including pre-seeded third-party handler and default-app associations that must survive.

## Out of scope
- User default-app selection (never changed; separate from thumbnails).
- The eventual MSI (Gate 7) — adopt the same identities and rules when it lands.
- Payload signing/SBOM (→ T42).

## Design notes
- Never set `DisableProcessIsolation` (ADR-0005).
- Use the exact CLSIDs/handler GUID from the design docs; do not regenerate any identity.
- Uninstall must never terminate or restart Explorer.

## Done when
- [ ] Registration/conflict/same-version-rerun/uninstall tests pass and the registry shape matches the T03-validated design.
- [ ] Registration is installer/script-owned only; the DLL exports no `DllRegisterServer`/`DllUnregisterServer`, so there is no self-registration path that bypasses the non-clobber rules.
- [ ] T34 has passed for supported STEP files. If it has not, T41 remains blocked until a scope-changing ADR and roadmap update define a different registration and release claim.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_

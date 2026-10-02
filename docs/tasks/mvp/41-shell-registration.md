---
verify: python tests/app-smoke/thumbnail_registration.py
---
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
- [x] Write `HKLM\Software\Classes\CLSID\{family}\InprocServer32` with `(Default)` = the installed DLL path and `ThreadingModel = Apartment` for all eight CLSIDs.
- [x] Implement the ShellEx mapping location validated by T03 for all direct extensions and verify effective routing with a third-party default ProgID and per-user association; add no `IPreviewHandler`, context-menu, property, icon-overlay or property-handler entry.
- [x] Implement non-clobber: if a pre-existing non-product handler occupies a value, record a conflict and preserve it. Define NSIS repair as an idempotent same-version installer rerun, since the current ARP entry sets `NoRepair=1`; rerun restores missing product files/keys without overwriting another handler.
- [x] Make uninstall remove only product-owned keys/values and broadcast `SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, null, null)` from a non-elevated notification action.
- [x] Add install/conflict/same-version-rerun/uninstall tests, including pre-seeded third-party handler and default-app associations that must survive.

## Out of scope
- User default-app selection (never changed; separate from thumbnails).
- The eventual MSI (Gate 7) — adopt the same identities and rules when it lands.
- Payload signing/SBOM (→ T42).

## Design notes
- Never set `DisableProcessIsolation` (ADR-0005).
- Use the exact CLSIDs/handler GUID from the design docs; do not regenerate any identity.
- Uninstall must never terminate or restart Explorer.

## Done when
- [x] Registration/conflict/same-version-rerun/uninstall tests pass and the registry shape matches the T03-validated design.
- [x] Registration is installer/script-owned only; the DLL exports no `DllRegisterServer`/`DllUnregisterServer`, so there is no self-registration path that bypasses the non-clobber rules.
- [x] T34 has passed for supported STEP files. If it has not, T41 remains blocked until a scope-changing ADR and roadmap update define a different registration and release claim.
- [x] Hand-off below filled in.

## Hand-off

### What landed
- `packaging/installer/Preview3DThumbnailRegistration.ps1` (new) is the single product-owned
  registration step. `-Action Install|Uninstall`, `-Scope HKLM|HKCU`, `-DllPath`,
  `-ClassesRoot`/`-StateRoot` overrides for tests. It writes, for the eight frozen family CLSIDs:
  `CLSID\{clsid}` default + `AppID`, `CLSID\{clsid}\InprocServer32` default = DLL path +
  `ThreadingModel=Apartment`, `AppID\{appid}` default + `DllSurrogate=""`; and for every direct
  extension `Software\Classes\<ext>\shellex\{E357FCCD-...}` = family CLSID. It never writes
  `DisableProcessIsolation` (and removes it if present), never reads/writes `UserChoice`, and adds no
  IPreviewHandler/context-menu/property/icon-overlay entry. The eight AppIDs are frozen by T41
  (ADR-0028); the CLSIDs/handler GUID come from the design docs and `FamilyRouting.h`.
- Non-clobber/ownership: a pre-existing non-product handler in an extension `ShellEx` value is
  preserved and recorded; ownership lives under `HKLM\Software\Binbuf\Preview3D\Thumbnails`
  (`OwnedClsid`/`OwnedAppId`/`OwnedShellEx`/`Conflicts`). Repair is an idempotent rerun. Uninstall
  removes a handler value only while it still equals a product CLSID, removes CLSID keys only while
  their `InprocServer32` is product-owned, and deletes the state key.
- `packaging/installer/Notify-Preview3DShellChanged.ps1` (new) P/Invokes
  `SHChangeNotify(SHCNE_ASSOCCHANGED /*0x08000000*/, SHCNF_IDLIST /*0*/, null, null)`. `Preview3D.nsi`
  runs it with a basic-user token (`runas /trustlevel:0x20000`) and still calls the existing
  `${NotifyShell_AssocChanged}` as a guaranteed fallback; failure is non-fatal.
- `packaging/installer/Preview3D.nsi`: stages and deletes both scripts, invokes the registration
  script elevated for Install and Uninstall, and keeps the Open With/ProgID registration unchanged.
- `packaging/portable/Create-PortableRelease.ps1`: stages both registration scripts for the
  `Installer` distribution only.
- `tests/app-smoke/thumbnail_registration.py` (new): static identity/contract check against
  `FamilyRouting.h` and the `.nsi`, plus a sandboxed HKCU lifecycle (pre-seeded foreign `.stl`/`.obj`
  handler and foreign `.stl` default/OpenWithProgids must survive install, repair rerun and
  uninstall).
- `tests/app-smoke/step_package.py` and `three_mf_package.py` now assert the family CLSID/extension
  and the T41 registration step instead of the previous "no shellex in NSIS" exclusion.
- Docs: new `docs/design/adr/0028-thumbnail-registration-non-clobber-step.md`; the
  `design/08-installation-and-registration.md` implementation note and key-shape/ownership sections
  describe the T41 step.

### Deviations and why
- Registration lives in a PowerShell step invoked by NSIS rather than inline `WriteRegStr`, because
  non-clobber conflict/repair/uninstall control flow cannot be expressed or tested in NSIS without an
  elevated install. ADR-0006/0007 allow an installer/script-owned vehicle and forbid only
  DLL self-registration.
- The per-family AppID values were frozen by T41 (the T22 smoke derived its own and the family
  hand-offs called them smoke-only). They match the T03/T22-validated shape.
- `SHCNE_ASSOCCHANGED`/`SHCNF_IDLIST` are the numeric constants `0x08000000`/`0` in the notification
  script because only a name is wanted in assertions; behaviour is identical.
- The provider DLL file is **not** added to the payload here: ADR-0007 assigns that (and the family
  runtime closure) to T42. Until T42 lands, `InprocServer32` names the stable install path
  `[INSTALLFOLDER]Preview3DThumbnailProvider.dll` but the file is not yet shipped; T42 must keep the
  `File` line for the DLL and its closure and keep staging the two registration scripts.

### Check results
- `python tests/app-smoke/thumbnail_registration.py` → exit 0:
  `thumbnail registration checks passed: static contract, sandboxed install/conflict/repair/uninstall lifecycle`.
- `python -c "...step_package/three_mf_package check_registration()"` → exit 0 for both.
- `makensis /PPO /NOCD /DSTAGE_DIR=C:\stage /DOUTPUT_FILE=C:\out.exe packaging/installer/Preview3D.nsi`
  → exit 0 (macros/functions/includes preprocess; Install/Uninstall invocations and both File/Delete
  lines present).
- Machine-level HKLM install elevation was not available in this non-interactive, non-elevated
  session; the installer path is exercised by T44 on a clean machine. The exact same script logic is
  exercised at HKCU sandbox scope by the test.

### Remaining work / next task must know
1. T42 must add `Preview3DThumbnailProvider.dll` and its family runtime closure to the NSIS payload
   (the `File` line in `Preview3D.nsi` and the `Create-PortableRelease.ps1` allowlist), keep the two
   registration scripts staged for the `Installer` distribution, and keep the DLL at the stable
   absolute install path recorded in `InprocServer32`.
2. T44 verifies the installed handler loads in `DllHost.exe` (not `explorer.exe`) with no
   `DisableProcessIsolation`, and that conflict/uninstall preserve a third-party handler on a clean
   machine.
3. The eventual MSI must adopt the same CLSIDs/AppIDs/extension mapping and non-clobber rules
   (ADR-0028).

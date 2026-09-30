# 0028 — Thumbnail registration is a testable non-clobber installer step

## Status
accepted

## Context
T03/ADR-0008 validated the per-user shape the Shell resolves — a plain
`InprocServer32`/`ThreadingModel=Apartment` handler plus the extension-level
`ShellEx` mapping — but T41 has to write it at machine scope from the NSIS
installer with non-clobber conflict handling, idempotent same-version repair,
uninstall that removes only product-owned values, and a non-elevated change
notification. Pure NSIS `WriteRegStr` gives no way to express "preserve a
foreign handler" and no way to unit-test the control flow without an elevated
install. The repository also never froze per-family AppIDs; the T03/T22 smoke
derived its own values and the family hand-offs flagged them as smoke-only.

## Decision
Machine-level thumbnail registration is a product PowerShell step,
`packaging/installer/Preview3DThumbnailRegistration.ps1`, invoked by
`Preview3D.nsi` (elevated, `-Action Install|Uninstall -Scope HKLM`) and also
directly by tests against a sandboxed per-user classes/state root.

- The eight CLSIDs, extensions and the handler GUID are mirrored from
  `thumbnail-provider/FamilyRouting.h`; `tests/app-smoke/thumbnail_registration.py`
  fails on any drift.
- One frozen AppID per family accompanies each CLSID with `DllSurrogate=""` for
  explicit `CLSCTX_LOCAL_SERVER` activation (the Shell thumbnail path isolates
  without it, ADR-0008).
- Ownership is recorded under `HKLM\Software\Binbuf\Preview3D\Thumbnails`
  (`OwnedClsid`/`OwnedAppId`/`OwnedShellEx`/`Conflicts`). A pre-existing
  non-product `ShellEx` value is preserved and recorded as a conflict; install
  and repair never overwrite it. Uninstall removes a handler value only when it
  still equals a product CLSID.
- Repair is an idempotent same-version installer rerun; the DLL payload itself
  lands in T42, so T41 only writes the registry shape.
- `Notify-Preview3DShellChanged.ps1` broadcasts
  `SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, null, null)`; the installer
  runs it with a basic-user token via `runas /trustlevel:0x20000` and still
  broadcasts from its own process as a guaranteed fallback.

Rejected: pure-NSIS registry writes (untestable non-clobber control flow);
`DllRegisterServer` self-registration (ADR-0006/ADR-0011).

## Consequences
- T42 must add `Preview3DThumbnailProvider.dll` and its closure to the payload
  and keep staging the two registration scripts; T44 verifies the installed
  handler in the surrogate on a clean machine.
- The eventual MSI adopts the same CLSIDs, AppIDs, extension mapping and
  non-clobber rules instead of a second identity set.
- A drift test, not a comment, keeps the installer table and `FamilyRouting.h`
  aligned.
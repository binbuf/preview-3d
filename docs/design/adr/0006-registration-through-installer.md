# 0006 — Thumbnail registration ships through the project's installer path

## Status
accepted

## Context
The provider baseline assumes the original WiX/MSI (Gate 7), which is not built. The scope-limited NSIS
installer exists but deliberately registers only viewer extensions and excludes thumbnails. Gate 6's
exit criteria nevertheless require machine-level CLSID/ShellEx registration with non-clobber conflict,
repair and uninstall behavior, verified on a clean installed machine.

## Decision
Thumbnail registration is implemented as an additive, machine-level registration step in the
project's current installer/packaging path (T41), written so the same component identities can be
adopted by the eventual MSI. It registers only this product's CLSIDs under
`HKLM\Software\Classes\CLSID` with `InprocServer32`/`ThreadingModel=Apartment`. The proposed
extension-level `ShellEx` mapping is provisional until T03 proves it routes with a third-party
default ProgID and a per-user association. T03 updates this ADR/design before T41 if that mapping
is insufficient. It never overwrites a pre-existing non-product handler: it records a
conflict and leaves the existing handler in place. Repair follows the same non-clobber rule unless the
existing value is one of this product's CLSIDs; uninstall removes only product-owned keys/values.
For NSIS, repair means an idempotent rerun of the same signed installer, not a Windows Installer
repair command. The eventual MSI provides transactional repair.

## Consequences
- T41 owns installer registration and its conflict/repair/uninstall tests; T44 owns clean-machine
  verification.
- The MSI (Gate 7) remains the long-term vehicle and must not diverge from these identities/rules.
- The provider DLL and every family it links must be present in the installed payload, which T42
  audits alongside signing and the SBOM.

# 0007 - The thumbnail provider program restores the installed scope through the NSIS installer

## Status
accepted

## Context
ADR-016 declared the "scope-limited MVP" to be a signed portable ZIP that
explicitly excludes the thumbnail provider, the per-machine MSI, registration,
and the compatibility host. Since that decision the repository has moved past
it: the shipping engineering package is the per-machine **NSIS** installer
(`packaging/installer/Preview3D.nsi`), it already stages the general import
worker, the OpenUSD compatibility host and the dedicated STEP host, and it
already registers Open With/Default Apps ProgIDs for all thirteen direct
extensions including `.step`/`.stp`. The one capability it still deliberately
omits is the Explorer thumbnail provider
(`packaging/CreateInstaller.proj` "Tests and the thumbnail provider remain
outside this payload"; `packaging/portable/Create-PortableRelease.ps1` forbids
`Preview3DThumbnailProvider.dll`). ADR-011 (signed per-machine MSI, seven
thumbnail CLSIDs) was never built.

The roadmap therefore covers a capability that ADR-016 says is out of scope,
and three distribution shapes (MSI, NSIS, portable ZIP) are referenced as if
they were the same vehicle. This program needs one explicit answer before
registration work (T41) is scheduled.

## Decision
This program **restores the installed original-MVP scope for the Explorer
thumbnail provider**. Concretely:

- The registration vehicle for the provider is the project's existing
  per-machine **NSIS installer path** (`packaging/installer/`), extended by
  T41 with machine-level `HKLM\Software\Classes\CLSID` /
  `...\shellex` thumbnail-handler registration exactly as ADR-0006 requires.
  The installer's current "thumbnail provider outside this payload" exclusion
  is removed for the provider DLL and its family runtime closure.
- The eventual **WiX/MSI** (delivery-plan Gate 7, ADR-011) remains the
  long-term vehicle and must adopt the same CLSID/ShellEx identities and the
  same non-clobber conflict/repair/uninstall rules; it is not a prerequisite
  for this program.
- The **portable ZIP** stays a scope-limited artifact: it does not gain the
  provider, registration, or thumbnails, and ADR-016's exclusion of the
  provider from the archive still holds. What is superseded is only ADR-016's
  claim that the *whole* limited MVP is portable-only; the importer worker,
  compatibility host and STEP host already ship in the NSIS installer.

ADR-016 is **superseded in part** by this record: its per-archive exclusions
stand, its "portable archive is the entire MVP" framing does not.

## Consequences
- `01-product-scope.md`, `08-installation-and-registration.md`,
  `overview.md` and `design/README.md` state the NSIS-now / MSI-later vehicle
  once, instead of carrying MSI, NSIS and portable claims side by side.
- T41 registers the eight CLSIDs and ShellEx handlers through the NSIS
  installer and its existing Create-Installer script; T42 extends that
  installer's payload allowlist with the provider DLL and its family runtime
  closure; T44 verifies the installed result on a clean machine.
- T52 records the retained Gate 6/7 checks against the NSIS-installed
  candidate and notes the MSI adoption as future work rather than an open
  dependency.
- ADR-011's "seven stable thumbnail CLSIDs" reads "eight", and the
  portable-ZIP SBOM/payload allowlists continue to forbid the provider.
# STEP-007 verification: product, package, and documentation integration

Status: **complete**
Completed: 2026-09-19
Design authority: [stp2.md](stp2.md) (STEP-007); [stp.md](stp.md);
[design/README.md](design/README.md)
Basis: [STEP-006-VERIFICATION.md](STEP-006-VERIFICATION.md)
Checked-in contract: [STEP-006-INTEROP-MATRIX.md](STEP-006-INTEROP-MATRIX.md)

## Result

STEP-007 exposes the completed static STEP/STP preview subset consistently
across the viewer, activation, and distribution surfaces without weakening the
handle-only/AppContainer boundary:

1. **Extension discovery.** `.step`/`.stp` are recognized case-insensitively by
   command-line validation, the Open dialog filters, drag/drop, secondary
   activation, Retry, supported-format errors, and the title-bar Open With
   catalog (catalog revision 7). `ClassifyByExtension` selects the dedicated
   STEP route, but the STEP host's `StepPart21Preflight` still verifies the
   ISO 10303-21 bytes before the CAD kernel runs, so an extension alone never
   bypasses STEP-002 admission.
2. **Viewer presentation.** The D3D12 bridge already routed STEP through
   `Preview3DStepHost.exe`; STEP-007 adds the bounded `StepProgress` wiring so
   the loading overlay shows product-owned phase text and N-of-M definition
   counts (opcode 21, closed `kStepPhase*`), plus the existing format,
   dimensions, verified bounds, units, and geometry/node/instance/material
   counts. No CAD tree, layer browser, or edit command was added.
3. **Packaging.** `Create-PortableRelease.ps1` stages the exact signed
   `StepHost` payload (the executable plus its closed 23-DLL constrained OCCT
   closure and app-local CRT) under `StepHost\`, copies the OCCT license from
   the STEP host's isolated vcpkg tree, adds the OCCT SBOM component, and
   validates the payload against a closed allowlist. `Preview3D.exe` and
   `Preview3DImportWorker.exe` remain OCCT-free.
4. **Registration.** NSIS installs `Binbuf.Preview3D.STEP.1` for `.step` and
   `.stp`, provisions the third `Binbuf.Preview3D.StepHost` AppContainer ACL,
   and removes only product-owned state on uninstall. It never writes the user's
   default and adds no thumbnail registration.
5. **Documentation.** README, portable/installer notes, About/help strings,
   supported-extension tables, the design support matrix/architecture/package
   documents, `TODO.md`, and public limitations now state the static preview
   subset, the external-reference no-go, and the lack of PMI/editing/Explorer
   thumbnails.

## Work item 1: extension discovery

Changed surfaces:

- [`D3D12ImportBridge.cpp`](../interactive-viewer/src/app/D3D12ImportBridge.cpp)
  `ClassifyByExtension` maps `.step`/`.stp` to `SourceFormat::Step`;
  `SourceFormatLabel` and the STEP-specific failure text already existed.
- [`ActiveInstance.cpp`](../interactive-viewer/src/app/ActiveInstance.cpp)
  `IsSupportedExtension` and the supported-format error text include both
  extensions for secondary activation and Retry.
- [`Preview3D.cpp`](../interactive-viewer/src/app/Preview3D.cpp) Open dialog
  filters add a `*.step;*.stp` entry to the "Supported 3D models" and STEP
  filters; the unsupported-format message lists them.
- [`ShellIntegration.cpp`](../interactive-viewer/src/platform/ShellIntegration.cpp)
  adds both extensions to `kSupportedExtensions` and bumps
  `kCatalogRevision` to 7 so the cached Open With catalog is invalidated.
- [`Reset-Preview3DTestAssociations.ps1`](../packaging/installer/Reset-Preview3DTestAssociations.ps1)
  cleans the STEP ProgID and extensions between installer tests.

## Work item 2: viewer presentation and progress

`d3d12_import_bridge::RunImport` now accepts an optional
`onStepProgress` callback and forwards it to
`ImportSessionRequest::onStepProgress` for `SourceFormat::Step` only.
`Preview3D.cpp` stores the last bounded phase/`definitionsMeshed`/
`definitionTotal` in atomics written by the import thread and read by the UI
thread, and `BuildOverlayInfo` appends fixed product-owned phase text (for
example "tessellating 3 of 10") while loading. The `--app-smoke` query fields
84/85/86 expose the phase and counts as qualification evidence. A STEP-host
fault seam (`stepHostArgumentsOverride` = `--pool-crash`/`--pool-hang`/
`--pool-overallocate`) was added to the viewer bridge so the real app can be
smoked through crash/timeout recovery.

## Work item 3: packaging

- [`Create-PortableRelease.ps1`](../packaging/portable/Create-PortableRelease.ps1)
  stages `StepHost\{Preview3DStepHost.exe, 23 OCCT DLLs, 5 CRT DLLs}`, copies
  `licenses\opencascade.txt` from
  `compatibility-host-step\vcpkg_installed`, adds `opencascade` to the SBOM
  (version/ABI read from that tree's `vcpkg.spdx.json`), signs the STEP host
  when a certificate is supplied, extends the PE system-DLL allowlist with
  `wsock32.dll`/`winmm.dll` (OCCT `TKernel`/`TKService` imports), and enforces a
  closed `StepHost` allowlist.
- [`Preview3D.nsi`](../packaging/installer/Preview3D.nsi) installs/removes
  `StepHost\` and its license and passes `-StepHostDirectory` to ACL
  provisioning.
- [`Provision-Preview3DWorkerAcl.ps1`](../packaging/installer/Provision-Preview3DWorkerAcl.ps1)
  now provisions three mutually isolated payload SIDs and verifies that no other
  importer SID retains access.
- [`Remove-Preview3DProfile.ps1`](../packaging/portable/Remove-Preview3DProfile.ps1)
  removes the third profile and its ACE.
- [`THIRD-PARTY-NOTICES.txt`](../packaging/portable/THIRD-PARTY-NOTICES.txt),
  [`PORTABLE-README.txt`](../packaging/portable/PORTABLE-README.txt),
  [`INSTALLER-README.txt`](../packaging/installer/INSTALLER-README.txt), and the
  root [`THIRD-PARTY-LICENSES.md`](../THIRD-PARTY-LICENSES.md) list OCCT.

## Work item 4: registration

`Preview3D.nsi` defines `Binbuf.Preview3D.STEP.1`, registers it for `.step`
and `.stp`, lists both under the application `SupportedTypes`/capabilities, and
symmetrically unregisters them and deletes the ProgID on uninstall. No
`UserChoice` write and no `shellex`/thumbnail registration is added.

## Work item 5: documentation

Updated: root [README.md](../README.md),
[interactive-viewer/README.md](../interactive-viewer/README.md),
[design/README.md](design/README.md),
[design/02-system-architecture.md](design/02-system-architecture.md),
[design/03-file-formats-and-ingestion.md](design/03-file-formats-and-ingestion.md),
[design/07-user-experience.md](design/07-user-experience.md),
[design/08-installation-and-registration.md](design/08-installation-and-registration.md),
[design/11-decisions-and-risks.md](design/11-decisions-and-risks.md) (ADR-017),
[TODO.md](TODO.md), and [PROGRESS.md](PROGRESS.md).

## Verification

### Real-app smoke (Debug and Release)

`python tests/app-smoke/step.py --configuration <Debug|Release>` passes the same
11 checks in both configurations:

```text
uppercase STEP direct command line with verified bounds and phase progress
AP214 part through secondary activation
AP242 authored tessellation through picker route
assembly through one-file drop route with node instances
malformed STEP retains prior usable content
required external STEP document fails typed without bypass
crash recovery preserves the later valid STEP open
timeout recovery preserves the later valid STEP open
pending STEP cancellation retains prior document
replacement publishes only the latest STEP generation
close and immediate STEP relaunch
```

The first check also asserts the closed Emit phase (`84 == 6`) and a nonzero
definition total (`86 > 0`), proving bounded progress reaches the viewer. The
smoke uses checked-in fixtures with SHA-256 recorded in the JSON results under
ignored `TestResults/step-007-smoke-{debug,release}.json`.

### Package contract

`python tests/app-smoke/step_package.py <stage>` verifies every manifest
SHA-256, the exact `StepHost` payload, that no OCCT DLL escaped to the viewer
root/worker/OpenUsdHost, the `opencascade` SBOM component and license, and the
symmetric `.step`/`.stp` NSIS registration. It passes against both the portable
stage (102 entries) and the installer stage (103 entries).

An unsigned engineering portable package
(`Create-PortableRelease.ps1`) and installer (`Create-Installer.ps1 -SkipBuild`)
both build; `makensis` compiles `Preview3D.nsi` with `/WX` (warnings as errors)
and the installer payload contains the `StepHost` tree.

### Regression

`x64/Release/Tests.ImportIsolation.exe "[step-002],[step-003],[step-004],[step-005],[step-006]"`
still passes **35 cases / 706 assertions**. The viewer Debug and Release
projects build with no new warnings.

## Known limits and handoff

- **No thumbnail registration.** Explorer STEP thumbnails remain STEP-009, as
  required; `.step`/`.stp` are direct-open and Open With types only.
- **No published Tier-B time budgets.** The genuine large-file corpus,
  time-to-first-coarse/Ready budgets, and performance regression gates remain
  STEP-008.
- **No clean-machine installer lifecycle.** The package contract is static;
  install/repair/upgrade/uninstall and signing on a clean VM remain STEP-008.
- **Healing stays off and external STEP documents stay out of scope**, per
  ADR-017.

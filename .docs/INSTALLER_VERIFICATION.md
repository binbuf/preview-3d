# Scope-limited NSIS installer verification

Date: 2026-09-16  
Configuration: Release x64  
Installer: NSIS 3.11

## Implemented surface

- Per-machine Windows 11 x64 install under `Program Files\Binbuf\Preview 3D`.
- Allowlisted viewer, isolated worker, app-local CRT/dependencies, notices,
  licenses, SBOM, and per-file manifest. The thumbnail provider,
  compatibility host, tests, PDBs, and debug runtime are rejected from staging.
- Stable ProgIDs for glTF (`.glb`, `.gltf`), STL (`.stl`), PLY (`.ply`),
  OBJ (`.obj`), and FBX (`.fbx`).
- Default Apps capabilities, `RegisteredApplications`, `OpenWithProgids`,
  `Applications\Preview3D.exe\SupportedTypes`, and App Paths registration.
- Quoted activation command: `"Preview3D.exe" --open "%1"`.
- Elevated setup provisions the deterministic import-worker AppContainer SID
  with inheritable read/execute access on only the private worker directory.
  Broad inherited application-package grants are removed from that directory;
  normal-user launches accept the pre-provisioned exact grant without needing
  `WRITE_DAC` under Program Files.
- Add/Remove Programs metadata, Start menu shortcuts, association-change
  notifications, running-viewer checks, and product-owned uninstall cleanup.
- Best-effort cleanup of the uninstalling user's import-worker AppContainer;
  preferences and source models are deliberately preserved.
- Finish-page handoff to the app-specific Windows 11 Default Apps page. Setup
  never writes or removes a protected per-user `UserChoice` value.

## Commands and results

The supported entry point completed successfully:

```powershell
msbuild Preview3D.slnx /t:CreateInstaller /p:Configuration=Release /p:Platform=x64
```

The human-facing script was also run with no arguments from outside the
repository. It inferred the repository from its own location, found
Visual Studio/MSBuild and NSIS, rebuilt Release x64, and emitted setup:

```powershell
powershell -ExecutionPolicy Bypass -File .\packaging\installer\Create-Installer.ps1
```

Results:

- Viewer and worker rebuilt successfully.
- Installer staging completed with 34 files.
- PE dependency closure validation found and corrected a pre-existing omission:
  `concrt140.dll` is now staged beside both importing executables.
- `makensis /WX` completed with no warnings.
- Output: `artifacts\installer\Preview3D-0.1.0-x64-setup.exe`.
- Engineering-build SHA-256:
  `b1ca08168d54571f5fb67c65dd830e0a9f12bec8e7bba20118f4e204e53b6523`.
- Adjacent `.sha256` verification matched the emitted setup executable.
- The portable solution target was rerun successfully after sharing the staging
  change; its stage contains both required `concrt140.dll` copies.
- The ACL helper was exercised on an isolated directory and left one exact
  Preview3D package-SID grant with read/execute plus object/container
  inheritance. Release import-isolation `[sandbox]` tests passed all 34
  assertions across six cases after rebuilding the changed platform code.

The output is intentionally reported as an unsigned engineering installer
because no signing thumbprint was supplied. The signing path covers the viewer,
worker, embedded uninstaller, and final setup executable when
`/p:InstallerSigningThumbprint=<SHA-1>` is provided.

## FBX-006 addendum (2026-09-17)

The installer now registers `Binbuf.Preview3D.FBX.1` through capabilities,
`OpenWithProgids`, and `Applications\Preview3D.exe\SupportedTypes`, with matching
uninstall cleanup and no thumbnail/shellex key. The association reset helper
preserves `UserChoice` and includes all six product ProgIDs, including the
previously omitted OBJ entry.

`packaging\CreateInstaller.proj` rebuilt the Release viewer/worker, staged 35
allowlisted files, and completed `makensis /WX`. The unsigned engineering setup
SHA-256 is
`375938af2dbd97ca6d7467785e1971d566329db75f1df9c05ca906b6fc8978f7`.
The corresponding 34-file portable archive SHA-256 is
`28e8344f768581975afc019bf20ee89e843dd17d69e10c93826a26d13d6d69ee`.
Both stages contain the pinned ufbx license, updated notice, and SBOM entry.
Clean-VM association lifecycle and signed-candidate checks remain below.

## Remaining release gates

Run clean Windows 11 x64 VM install/upgrade/uninstall tests as both an
interactive administrator and a standard user supplying elevation. Confirm all
all supported extensions appear on the app-specific Default Apps page, select
them,
open adversarial quoted/Unicode paths, verify worker isolation, and confirm
uninstall removes product-owned registration without changing unrelated
defaults. Also verify signed-file trust and hashes when a release certificate is
available. No clean-VM lifecycle or signed-candidate claim is made by this
engineering build.

## USD-008 addendum (2026-09-18)

The installer now owns one stable `Binbuf.Preview3D.USD.1` ProgID for `.usd`,
`.usda`, `.usdc`, and `.usdz`, wired through capabilities, OpenWithProgids,
SupportedTypes, reset, and uninstall. No USD CLSID, `shellex`, thumbnail
handler, or `UserChoice` mutation is present.

The product stage contains the separate 26-file `OpenUsdHost` closure with the
bootstrap/core, exact runtime DLLs and app-local CRT, and the 13 hash-audited
OpenUSD resources. The packaging script compares that recursive inventory with
an explicit allowlist, validates PE closure (including OpenUSD's Windows-system
`dbghelp.dll` and `shlwapi.dll` imports), hashes it in the release manifest,
and adds OpenUSD/oneTBB license and SBOM entries. The installer ACL helper was
exercised against isolated directories and verified mutually exclusive
read/execute grants for `Binbuf.Preview3D.ImportWorker` and
`Binbuf.Preview3D.ImportHost`.

The portable engineering stage contains 63 files; its archive SHA-256 is
`eaba718e74b2681fa6d9d14aa76d66f9a0496b511ea9818d9a46da5df27c484d`.
A composed USD fixture opened successfully from that staged layout. The NSIS
stage contains 64 files, `makensis /WX` completed without warnings, and the
unsigned engineering installer SHA-256 is
`b8a25b44f2d0d2e0daeac5ae59492862e8c38deaa1d2ad790f8ce0de4248f4f8`.
Clean-VM association/ACL lifecycle and signed-candidate verification remain
USD-009 release gates.

## 3MF-006 addendum (2026-09-18)

The installer now owns `Binbuf.Preview3D.ThreeMF.1` for `.3mf` through
capabilities, OpenWithProgids, and SupportedTypes. Uninstall and the
association-reset helper remove only product-owned entries; neither changes
`UserChoice` or registers a 3MF thumbnail/shell extension.

The worker-only 3MF runtime closure is `lib3mf.dll`, `zip.dll`, `z.dll`, and
`bz2.dll`; the viewer has no 3MF/ZIP parser dependency. Portable and NSIS
stages include those four DLLs, corresponding license and SBOM entries, and
hashes in `MANIFEST.json`. A static package-contract check verifies all staged
file hashes and symmetric registration/removal. The portable engineering stage
contains 73 files (archive SHA-256
`40a19df6e935b8608e94fa1c22e583e95cfddec6406722f07bc66013239e7311`);
the NSIS stage contains 74 files (`makensis /WX` succeeded; unsigned setup
SHA-256 `d74433c2313c16719c914b0eaa6781a663d2138314d74f1991ec6aef7d88dbae`).
The real viewer opened and recovered from 3MF in the staged portable layout.
Actual clean-VM install/repair/upgrade/uninstall, association lifecycle, and
signed-candidate verification remain 3MF-007 release gates.

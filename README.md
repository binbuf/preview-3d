# 3D Preview

A small native Windows 11 viewer for local GLB/glTF, OBJ/MTL, ASCII or binary
STL, and ASCII or binary PLY mesh/point files. Imports run in a zero-capability
AppContainer worker; the viewer does not upload, edit, or modify models.

## Build and run

Open `Preview3D.slnx` in Visual Studio with the Desktop C++ workload and vcpkg
enabled, or build `Release|x64` with MSBuild. Run `x64\Release\Preview3D.exe`
and use Ctrl+O, drag one supported file onto the window, or pass a path on the
command line.

The title-bar X/Y/Z control cycles which model axis is treated as up and mapped
to the ground plane. The adjacent direction button flips between the positive
and negative side of that axis, so upside-down source models can be grounded on
their feet. Each choice immediately becomes the Reset/Home view and is
remembered for later launches.

The scope-limited release has explicit portable and installer solution targets:

```powershell
msbuild Preview3D.slnx /t:CreatePortableRelease /p:Configuration=Release /p:Platform=x64
msbuild Preview3D.slnx /t:CreateInstaller /p:Configuration=Release /p:Platform=x64
```

For the direct installer workflow, run this from any working directory; it
finds the repository, Visual Studio/MSBuild, and NSIS automatically, builds the
current Release x64 binaries, and emits setup under `artifacts\installer`:

```powershell
powershell -ExecutionPolicy Bypass -File .\packaging\installer\Create-Installer.ps1
```

If development builds or earlier Open With tests left per-user registration
that shadows the installed command, preview and reset only Preview3D's test
association state with:

```powershell
.\packaging\installer\Reset-Preview3DTestAssociations.ps1 -WhatIf
.\packaging\installer\Reset-Preview3DTestAssociations.ps1
```

The portable target writes one clean archive and SHA-256 to `artifacts\portable`.
The installer target requires NSIS 3 and writes a setup executable and SHA-256
to `artifacts\installer`; override NSIS discovery with
`/p:NsisPath=<path-to-makensis.exe>` when needed. Unsigned output is explicitly
an engineering build. Signed candidates additionally supply
`/p:PortableSigningThumbprint=<certificate SHA-1>` or
`/p:InstallerSigningThumbprint=<certificate SHA-1>`. Final signing and clean-VM
acceptance remain release gates. See [portable package notes](packaging/portable/PORTABLE-README.txt),
[installer notes](packaging/installer/INSTALLER-README.txt), and
[portable verification](.docs/TSK-303_VERIFICATION.md).

The installer registers 3D Preview in Windows Default Apps and Open With for
`.glb`, `.gltf`, `.obj`, `.stl`, and `.ply`, and offers the Windows 11 confirmation page
after setup. Windows protects per-user default choices, so setup does not alter
an existing `UserChoice` value. Explorer thumbnails, persistent model-derived
cache, compatibility-host/USD support, and the remaining Tier B formats remain excluded.
Large-model performance failures recorded in
[progress](.docs/PROGRESS.md) also remain release blockers.

## Releases

GitHub Actions builds and publishes a release only when a tag matching
`vMAJOR.MINOR.PATCH` is pushed. SemVer prerelease and build suffixes are also
accepted (for example, `v0.2.0-rc.1`). The tag version is embedded in the
portable archive, installer metadata, SBOM, manifest, and artifact names.

```powershell
git tag v0.2.0
git push origin v0.2.0
```

The release contains the portable ZIP, the NSIS installer, and a SHA-256 file
for each. Builds are unsigned unless both `WINDOWS_CERTIFICATE_BASE64` (a
base64-encoded PFX) and `WINDOWS_CERTIFICATE_PASSWORD` are configured as GitHub
Actions repository secrets. When present, the workflow temporarily imports the
certificate and signs both payloads and the installer.

## Performance

   Fixture        Previous complete coarse    New p95
  ━━━━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━━━━
   3.00 GB STL                     12.47 s     4.19 s
  ─────────────  ──────────────────────────  ─────────
   2.88 GB PLY                     11.44 s     4.67 s
  ─────────────  ──────────────────────────  ─────────
   4.29 GB GLB                       >30 s     5.22 s
  ─────────────  ──────────────────────────  ─────────
   A-small                         ~0.46 s     0.47 s

## Credit

<a href="https://www.flaticon.com/free-icons/geometric" title="geometric icons">Geometric icons created by Magnific - Flaticon</a>

<a href="https://www.flaticon.com/free-icons/perspective" title="perspective icons">Perspective icons created by Iconir - Flaticon</a>

<a href="https://www.flaticon.com/free-icons/grid" title="grid icons">Grid icons created by Magnific - Flaticon</a>

<a href="https://www.flaticon.com/free-icons/speed" title="speed icons">Speed icons created by Magnific - Flaticon</a>

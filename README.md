# Preview 3D

A fast optimized 3D viewer for Windows 11.

[Download the latest release](https://github.com/binbuf/preview-3d/releases/latest) · [Report an issue](https://github.com/binbuf/preview-3d/issues) · [Windows security help](.docs/WINDOWS-SECURITY.md)

## Highlights

- Open local GLB/glTF, OBJ/MTL, FBX, STL, PLY, 3MF, USD-family, and STEP/STP files.
- Navigate with familiar orbit, pan, fly, frame, and orthographic-view controls.
- Drag and drop files, use **Open**, or pass a path on the command line.
- Run parsing and decoding in a zero-capability AppContainer worker; models stay local and are never modified.
- Install file associations for supported formats, or use a portable ZIP with no installer.

## Supported formats

| Format | Support |
| --- | --- |
| glTF 2.0 | `.glb` and `.gltf`, including local relative binary and image sidecars |
| Wavefront OBJ | `.obj` with optional local `.mtl` and texture sidecars |
| FBX | Binary or ASCII `.fbx`, including static hierarchy, instances, supported materials/textures, and a deterministic baked start pose |
| STL | ASCII and binary |
| PLY | ASCII and binary triangle meshes and point clouds |
| 3MF | The supported static `.3mf` preview subset: Core geometry/components/build items, Materials and Properties colors/textures, Production model parts, and bounded Beam Lattice previews |
| Universal Scene Description | `.usd`, `.usda`, `.usdc`, and `.usdz`; static meshes, hierarchy/instances, common primvars, display color, bounded USD Preview Surface materials/textures, and bounded local composition |
| STEP | `.step` and `.stp` self-contained ISO 10303-21 AP203/AP214/AP242 B-rep or authored AP242 tessellated geometry, assemblies/reused definitions, instance/shape/face colors, and authored length units through a dedicated isolated OCCT host |

USD files first use TinyUSDZ in the general isolated importer. Stages requiring
supported composition are retried atomically in a separately isolated, lazily
started OpenUSD host. Local relative sublayers, references, payloads, authored
default variants, and texture dependencies are brokered by the viewer; remote
assets and arbitrary resolvers/plugins are never allowed.

STEP/STP uses a third, dedicated zero-capability host with a pinned constrained
OCCT closure. It receives only an inherited read-only handle (never a path),
performs product-owned Part-21 admission before the CAD kernel runs, and emits
only normalized wire records. The route is case-insensitive on command line,
dialog, drag/drop, single-instance activation, Retry, and Open With.

The implemented USD viewer path has an immutable corpus and a standalone
sanitizer fuzz-smoke lane. Final release qualification still requires the
recorded clean-machine, repeated performance/heartbeat, soak, and signed-build
gates; see [USD-009 verification](.docs/USD-009-VERIFICATION.md). Explorer USD
thumbnails are a separate follow-up and are not installed.

This is a static, read-only viewer. Animation playback, editing, other CAD
formats (IGES/IFC/JT/native CAD), Explorer thumbnails (including for 3MF, USD,
FBX, and STEP), network assets,
skeletal USD data, MaterialX, procedural schemas, and interactive variant
selection are not currently included. The supported STEP/STP subset is bounded
and self-contained only: external STEP documents are out of scope, and there is
no PMI/GD&T, editing, saved views, exact measurement, or shape healing. 3MF slicer-private multi-plate grouping,
printer/process settings, Slice, Secure Content, Volumetric, Implicit, toolpath,
repair, slicing, and export features are not supported. 3MF and USD are bounded
Tier B paths: among their ceilings are 2 GiB per primary source, 4 GiB
aggregate local source/archive expansion, and 20 million triangles or points.
The separate USD compatibility host has a commit cap of the lower of 4 GiB or
35% of physical memory.

For preview compatibility, a mesh labeled Core object type `other` is shown if
it is referenced by the build, as some slicers produce this even though the 3MF
Core specification disallows it. Slicer plate grouping and print settings do
not affect the preview; a multi-plate project may show all root-build objects
together. The 3MF build placement is preserved, including models positioned
head-down for printing. To view one upright on the grid, select ground axis Z
and use the ground direction button to make negative Z point up.

## Install and use

1. Download the installer or portable ZIP from [Releases](https://github.com/binbuf/preview-3d/releases/latest).
2. For the portable ZIP, extract it and keep `worker`, `OpenUsdHost`, and `StepHost` beside `Preview3D.exe`.
3. Open a model with `Ctrl+O`, drag a supported file onto the window, or run `Preview3D.exe <path-to-model>`.

The installer adds Preview 3D to **Open with** and **Default apps** for the supported extensions. Windows keeps existing default-app choices; confirm any changes in Default apps after installation.

> [!IMPORTANT]
> Releases are currently **unsigned** while code-signing and reputation work is in progress, so Windows may block the app or one of the DLLs bundled beside it (for example the Bad Image status `0xC0E90002`). Only download from this repository’s Releases page and verify the supplied SHA-256 checksum. For the portable ZIP, right-click the downloaded file, choose **Properties**, and select **Unblock** *before* extracting, so its contents do not inherit the mark. Smart App Control has no per-file exception; see [Windows security help](.docs/WINDOWS-SECURITY.md) for the specific, safe steps to allow a release you have verified.

## Build from source

### Prerequisites

- Windows 11 x64.
- Visual Studio with the **Desktop development with C++** workload (the `v145` MSVC toolset, C++20) and a Windows 10/11 SDK. The `.slnx` solution format needs a recent Visual Studio release.
- [vcpkg](https://learn.microsoft.com/vcpkg/get_started/get-started). Visual Studio ships one at `<Visual Studio>\VC\vcpkg`; a standalone clone works too.
- NSIS 3, only for the installer target.

### Set up on a new workstation

The pinned dependency versions live in `vcpkg.json` and `vcpkg-configuration.json`, but the compiled dependencies are **not** committed — `vcpkg_installed/` is gitignored. A fresh clone therefore has to restore them once, and MSBuild only does that when vcpkg integration is installed for your user. Using the vcpkg you intend to build with (the Visual Studio-bundled copy lives at `<Visual Studio>\VC\vcpkg`):

```powershell
vcpkg integrate install
```

Then open `Preview3D.slnx`, pick **Release | x64** (or **Debug | x64**), and build. The first build runs `vcpkg install` against the root manifest and populates `vcpkg_installed\x64-windows-static-md`. Release builds statically link the dependency closure (`VcpkgUseStatic`/`VcpkgUseMD` in `Directory.Build.props`) so the shipped payload contains no upstream DLLs — only the app's own images. The OpenUSD overlay port (`packaging/vcpkg-ports/openusd`) and the STEP host's separate OCCT manifest (`compatibility-host-step\vcpkg.json`, installed under `compatibility-host-step\vcpkg_installed`) compile from source, so this first restore takes **tens of minutes to a few hours**; later builds reuse the archives under `%LOCALAPPDATA%\vcpkg\archives`.

If the build cannot find a third-party header instead of restoring, the integration is not active for the MSBuild you are running:

```
error C1083: Cannot open include file: 'pxr/base/plug/plugin.h'
error C1083: Cannot open include file: 'fastgltf/core.hpp'
```

Re-run `vcpkg integrate install` with the vcpkg you build with, then rebuild. (With a standalone vcpkg, make sure `VCPKG_ROOT` points at it and that no other vcpkg is integrated.)

### Command line

```powershell
msbuild Preview3D.slnx /p:Configuration=Release /p:Platform=x64
msbuild Preview3D.slnx /t:CreatePortableRelease /p:Configuration=Release /p:Platform=x64
msbuild Preview3D.slnx /t:CreateInstaller /p:Configuration=Release /p:Platform=x64
```

The first command builds the viewer, worker, and both compatibility hosts. The second writes the portable archive and checksum to `artifacts\portable`; the third requires NSIS 3 and writes the installer to `artifacts\installer`. See the [portable package notes](packaging/portable/PORTABLE-README.txt) and [installer notes](packaging/installer/INSTALLER-README.txt) for release and cleanup details.

## Project notes

- [Windows download and protection guidance](.docs/WINDOWS-SECURITY.md)
- [Installer verification](.docs/INSTALLER_VERIFICATION.md)
- [Release workflow](.github/workflows/release.yml)

## Credits

Interface icons: [Magnific](https://www.flaticon.com/free-icons/geometric), [Iconir](https://www.flaticon.com/free-icons/perspective), [Magnific](https://www.flaticon.com/free-icons/grid), and [Magnific](https://www.flaticon.com/free-icons/speed) via Flaticon.

## License

Copyright 2026 Binbuf. Preview3D is licensed under the [Apache License 2.0](LICENSE).
See [NOTICE](NOTICE) for required attribution notices and
[third-party license information](THIRD-PARTY-LICENSES.md) for dependencies.

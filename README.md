# Preview 3D

A fast optimized 3D viewer for Windows 11.

[Download the latest release](https://github.com/binbuf/preview-3d/releases/latest) · [Report an issue](https://github.com/binbuf/preview-3d/issues) · [Windows security help](.docs/WINDOWS-SECURITY.md)

## Highlights

- Open local GLB/glTF, OBJ/MTL, FBX, STL, and PLY files.
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

This is a static, read-only viewer. Animation playback, editing, USD/3MF/CAD
formats, Explorer thumbnails (including for FBX), and network assets are not
currently included.

## Install and use

1. Download the installer or portable ZIP from [Releases](https://github.com/binbuf/preview-3d/releases/latest).
2. For the portable ZIP, extract it and keep `worker` beside `Preview3D.exe`.
3. Open a model with `Ctrl+O`, drag a supported file onto the window, or run `Preview3D.exe <path-to-model>`.

The installer adds 3D Preview to **Open with** and **Default apps** for the supported extensions. Windows keeps existing default-app choices; confirm any changes in Default apps after installation.

> [!IMPORTANT]
> Windows may flag a new or unsigned release while code-signing and reputation work is in progress. Only download from this repository’s Releases page and verify the supplied SHA-256 checksum. See [Windows security help](.docs/WINDOWS-SECURITY.md) for safe, specific steps—including the difference between a file’s **Unblock** checkbox and Smart App Control.

## Build from source

Open `Preview3D.slnx` in Visual Studio with the **Desktop development with C++** workload and vcpkg available, then build `Release | x64`.

```powershell
msbuild Preview3D.slnx /t:CreatePortableRelease /p:Configuration=Release /p:Platform=x64
msbuild Preview3D.slnx /t:CreateInstaller /p:Configuration=Release /p:Platform=x64
```

The first command writes the portable archive and checksum to `artifacts\portable`; the second requires NSIS 3 and writes the installer to `artifacts\installer`. See the [portable package notes](packaging/portable/PORTABLE-README.txt) and [installer notes](packaging/installer/INSTALLER-README.txt) for release and cleanup details.

## Project notes

- [Windows download and protection guidance](.docs/WINDOWS-SECURITY.md)
- [Installer verification](.docs/INSTALLER_VERIFICATION.md)
- [Release workflow](.github/workflows/release.yml)

## Credits

Interface icons: [Magnific](https://www.flaticon.com/free-icons/geometric), [Iconir](https://www.flaticon.com/free-icons/perspective), [Magnific](https://www.flaticon.com/free-icons/grid), and [Magnific](https://www.flaticon.com/free-icons/speed) via Flaticon.

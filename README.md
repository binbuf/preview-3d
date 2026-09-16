# 3D Preview

A small native Windows 11 viewer for local GLB/glTF, binary STL, and binary
PLY mesh/point files. Imports run in a zero-capability AppContainer worker;
the viewer does not upload, edit, or modify models.

## Build and run

Open `Preview3D.slnx` in Visual Studio with the Desktop C++ workload and vcpkg
enabled, or build `Release|x64` with MSBuild. Run `x64\Release\Preview3D.exe`
and use Ctrl+O, drag one supported file onto the window, or pass a path on the
command line.

The scope-limited portable package has an explicit solution target:

```powershell
msbuild Preview3D.slnx /t:CreatePortableRelease /p:Configuration=Release /p:Platform=x64
```

It writes one clean archive and its SHA-256 to `artifacts\portable`. Without a
certificate it is visibly marked as an unsigned engineering archive. A signed
candidate additionally supplies
`/p:PortableSigningThumbprint=<certificate SHA-1>`; final signing and clean-VM
acceptance remain release gates. See
[portable package notes](packaging/portable/PORTABLE-README.txt) and
[current verification](.docs/TSK-303_VERIFICATION.md).

The limited MVP deliberately excludes installation/file associations, Explorer
thumbnails, persistent cache, compatibility-host/USD support, and broad Tier B
formats. Large-model performance failures recorded in
[progress](.docs/PROGRESS.md) also remain release blockers.

## Credit

<a href="https://www.flaticon.com/free-icons/geometric" title="geometric icons">Geometric icons created by Magnific - Flaticon</a>

<a href="https://www.flaticon.com/free-icons/perspective" title="perspective icons">Perspective icons created by Iconir - Flaticon</a>

<a href="https://www.flaticon.com/free-icons/grid" title="grid icons">Grid icons created by Magnific - Flaticon</a>

<a href="https://www.flaticon.com/free-icons/speed" title="speed icons">Speed icons created by Magnific - Flaticon</a>

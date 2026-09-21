Preview3D @VERSION@ portable engineering package (Windows 11 x64)
================================================================

Run Preview3D.exe and use Ctrl+O, or pass one local model path on the command
line. Keep the worker directory beside Preview3D.exe. The app does not require
administrator rights, write file associations, or modify the opened model.
Keep the OpenUsdHost directory beside Preview3D.exe as well; it is the private
compatibility payload for composed USD stages. Keep the StepHost directory
beside Preview3D.exe too; it is the private CAD kernel payload for STEP/STP.

Supported content
-----------------

* GLB and glTF 2.0 with local relative binary/image sidecars.
* OBJ polygon meshes with optional local MTL and texture sidecars.
* Binary and ASCII FBX with static hierarchy/instances, supported materials and
  textures, and a deterministic baked start pose for supported skin/blend data.
* ASCII and binary STL.
* ASCII and binary little- or big-endian PLY triangle meshes and point clouds.
* The supported static .3mf preview subset: Core geometry, components, all root
  build items, Materials and Properties colors/textures, Production model
  parts, and bounded Beam Lattice previews.
* .usd, .usda, .usdc, and .usdz static stages. The supported subset includes
  meshes, hierarchy, instances/point instances, transforms, common primvars,
  display color, material subsets, USD Preview Surface factors/textures, and
  bounded local sublayers, references, inherits/specializes, authored default
  variants, and payloads.
* Self-contained .step and .stp ISO 10303-21 files (AP203/AP214/AP242) with
  bounded B-rep or authored AP242 tessellated geometry, assemblies and reused
  definitions, instance/shape/face colors, and authored length units. External
  STEP documents are not supported.
* The bounded glTF subset includes static meshes/instances, vertex colors,
  metallic/roughness materials, normal/emissive textures, unlit materials,
  alpha modes, KHR_texture_transform, KHR_draco_mesh_compression,
  KHR_mesh_quantization, EXT_meshopt_compression, KHR_texture_basisu/KTX2,
  and EXT_texture_webp.

Important limits
----------------

This is a local, read-only static viewer. Animation playback, editing, network
assets, other CAD formats (IGES/IFC/JT/native CAD), thumbnails (including
3MF/USD/FBX/STEP Explorer thumbnails), file associations, and a
persistent derived cache are outside this limited MVP. Optional unsupported
glTF material/image features may fall back with a warning; required unsupported
extensions fail. OBJ supports faces, triangulation, smoothing/generated normals,
UVs, vertex colors, object/group meshes, MTL factors, and broker-approved
base-color, normal/bump, and emissive maps. Lines, curves, animation, and distinct
roughness/metalness texture maps are not rendered. Imports and decoded data are
bounded; over-limit or malformed models fail instead of rendering partially.
FBX geometry caches, dynamic constraints, NURBS/subdivision tessellation,
cameras, and lights are not rendered.
3MF slicer-private multi-plate grouping and settings are ignored; all standard
root-build items are shown together in authored coordinates. Slice, Secure
Content, Volumetric, Implicit, toolpath, repair, slicing, and export features
are not supported. Unsupported required extensions fail rather than presenting
an unfaithful preview.
USD animation, skeletal data, MaterialX, procedural schemas, arbitrary plugins,
remote assets, and interactive variant selection are not supported. USD uses
Tier B ceilings including 2 GiB primary source, 4 GiB aggregate local bytes and
USDZ expansion, 20 million triangles or points, 50,000 nodes, and bounded
counts for materials/textures/dependencies. The OpenUSD host is additionally
limited to the lower of 4 GiB or 35% of physical memory.
STEP files are limited to self-contained ISO 10303-21 content: required
external documents, PMI/GD&T, saved views, editing, and exact measurement are
not supported, and tessellation uses a fixed bounded quality policy. The STEP
host is additionally limited to the lower of 4 GiB or 35% of physical memory.

The worker runs in a zero-capability AppContainer. On first import, Preview3D
creates the current-user profile Binbuf.Preview3D.ImportWorker and grants that
profile read/execute access only to this package's worker directory. Model and
sidecar data are supplied as read-only handles by the viewer; the worker is not
granted access to the model directory. The profile creation and ACL update need
no elevation. Deleting the extracted directory removes the granted payload.
Composed USD stages use a second zero-capability profile,
Binbuf.Preview3D.ImportHost. It can read/execute only OpenUsdHost, starts lazily,
exits after the generation, and receives local relative dependencies only as
brokered bytes. STEP/STP uses a third zero-capability profile,
Binbuf.Preview3D.StepHost. It can read/execute only StepHost, receives the
source as a read-only inherited handle (never a path), and exits after the
generation. None of the importers can read another's private directory.

Windows may block this build
----------------------------

This engineering build is unsigned, so Windows can refuse to start Preview3D.exe
or one of the DLLs beside it with a Bad Image error (for example
worker\zstd.dll, status 0xC0E90002).

* Before extracting the ZIP, right-click it, choose Properties, and select
  Unblock, so the extracted files do not inherit the downloaded-file mark.
* If the ZIP is already extracted, unblock everything in this folder from
  PowerShell:  Get-ChildItem -Recurse | Unblock-File
* If SmartScreen shows "Windows protected your PC", choose More info and then
  Run anyway, after verifying the adjacent .sha256 checksum.
* Smart App Control has no per-file exception. If it blocks the app, turn Smart
  App Control off (Windows Security > App & browser control > Smart App Control
  settings) for a build you have verified. See the project's
  .docs/WINDOWS-SECURITY.md.

Cleanup
-------

Close Preview3D first. To also remove the per-user AppContainer profile and its
worker-directory ACL entry, run:

  powershell -NoProfile -ExecutionPolicy Bypass -File .\Remove-Preview3DProfile.ps1

No daemon, service, shell extension, registry file association, or cache is
installed by this package.

Runtime and support
-------------------

The archive contains the required app-local MSVC runtime, the app's own
executables with their dependency closure statically linked, and the exact
private OpenUSD schema/plugin resources. Direct3D 12, DXGI,
Direct2D, DirectWrite, WIC, and D3DCompiler 47 are
Windows 11 system components and are not redistributed. A Direct3D feature
level 11-capable adapter/driver is required. Coarse/full rendering intentionally
uses bounded representative geometry and view-driven refinement; it is not an
authoring-fidelity or complete-scene residency guarantee.

MANIFEST.json records every other payload's SHA-256. SBOM.cdx.json records
component versions and vcpkg provenance. THIRD-PARTY-NOTICES.txt and licenses\ contain
redistribution notices. The ZIP's adjacent .sha256 file verifies the archive.
An archive whose manifest says "signed": false is an engineering build and is
not a release candidate.

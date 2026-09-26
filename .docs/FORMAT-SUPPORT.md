# Format support and limits

Preview 3D is a static, read-only viewer. This document describes the exact
supported subset, isolation model, and known limitations for each format.
For install and build instructions, see the [README](../README.md).

## Support matrix

| Format | Support |
| --- | --- |
| glTF 2.0 | `.glb` and `.gltf`, including local relative binary and image sidecars, bounded Draco/meshopt geometry, mesh quantization, KTX2/Basis, PNG/JPEG/WebP textures, material texture slots, texture transforms, `KHR_materials_pbrSpecularGlossiness` approximated as diffuse albedo plus a dielectric response, and `KHR_materials_transmission`/`KHR_materials_ior` approximated as Fresnel-weighted glass (an authored index of refraction at or below 1 keeps the surface opaque, since there is no optical interface) |
| Wavefront OBJ | `.obj` with optional local `.mtl` and texture sidecars; ufbx polygon triangulation, smoothing/generated normals, UVs, vertex colors, object/group meshes, MTL material factors, and broker-approved base-color, normal/bump, and emissive maps |
| FBX | Binary or ASCII `.fbx`, including static hierarchy, instances, supported materials/textures, and a deterministic baked start pose; supported skin and blend deformation is baked. Image references that carry the authoring machine's absolute path are reduced to their file name and resolve through the package lookup below |
| STL | ASCII and binary |
| PLY | ASCII and binary triangle meshes and point clouds |
| 3MF | The supported static `.3mf` preview subset: Core geometry/components/build items, Materials and Properties colors/textures, Production model parts, and bounded Beam Lattice previews |
| Universal Scene Description | `.usd`, `.usda`, `.usdc`, and `.usdz`; static meshes, hierarchy/instances, common primvars, display color, bounded USD Preview Surface materials/textures, and bounded local composition |
| STEP | `.step` and `.stp`: self-contained ISO 10303-21 AP203/AP214/AP242 B-rep or authored AP242 tessellated geometry, assemblies/reused definitions, instance/shape/face colors, and authored length units through a dedicated isolated OCCT host |

`.mtl` is always a sidecar and is never a primary open type. The Open dialog,
command line, drag/drop, single-instance activation, Retry, and Open With all
accept the same direct formats, case-insensitively.

## Downloaded-package texture layout

Many downloaded models use a package layout rather than keeping every file
beside the model:

```
model/
  source/model.fbx          # or .glb/.gltf/.obj/.usd/.usdc/...
  textures/*.png            # sibling folder
```

```
model/
  model.gltf                # model at the package root
  texture/*.png             # or textures/, singular or plural
  pattern.png               # images beside the model
```

For a local file import, a sidecar image reference that does not resolve
directly is looked up, in this exact order:

1. beside the model;
2. `<model dir>/texture`;
3. `<model dir>/textures`;
4. `<parent>/texture`;
5. `<parent>/textures`.

The first directory with a matching image wins, so the closest copy is
preferred and two model packages in the same parent cannot collide. The search
is **image-only** (`.png`, `.jpg`, `.jpeg`, `.bmp`, `.tif`, `.tiff`, `.webp`,
`.ktx2`); a missing `.bin` buffer, `.mtl`, or USD layer is never resolved this
way. Names are matched exactly first, then with case, space/underscore, and
`jpg`/`jpeg`/`tif`/`tiff` spelling normalized, plus material-role
abbreviations (`_BaseColor`/`_B`, `_Normal`/`_N`, `_Roughness`/`_R`,
`_Emissive`/`_E`, `_Metallic`/`_M`, `_Glossiness`/`_G`) and a trailing download
annotation such as `_(Personalizado)`, so files renamed by a download site
still match. A name containing wildcards is never used as a pattern, and the
matched file still passes the same canonical containment and size checks as any
other sidecar. Traversal, UNC/device, alternate-data-stream, and URL references
are rejected before any search and never resolve this way. The authored path
itself is never opened.

FBX additionally recovers a material whose base color was not wired at all but
whose map sits in a texture folder: the material name is used to ask for
`<Name>_Base_color.png`/`.jpg`, resolved through the same package lookup (one
or two bounded requests per material, never enough to threaten the 64-request
generation cap). The inferred map is best-effort and can never fail a model
whose authored maps already fit.

The aggregate decoded-texture budget is bounded per generation. When a
texture-heavy package reaches it, remaining maps are downscaled (down to a
small preview resolution) so the model still loads with as many maps as fit,
instead of failing; per-texture resolution is still capped and validated by the
worker and the broker independently. A glTF image URI that contains `..` (what
exporters write for a model inside `source/`) is reduced to its file name
before the request, so the authored traversal is never followed; required
buffers and USD layers keep the strict rejection.

## Isolation model

All general parsing and decoding runs in `Preview3DImportWorker.exe` under a
zero-capability AppContainer, and models reach the viewer as validated
wire-format chunks. Local relative sidecars are brokered by the viewer; remote
assets and arbitrary resolvers/plugins are never allowed.

USD files first use TinyUSDZ in the general isolated importer. Stages requiring
supported composition are retried atomically in a separately isolated, lazily
started OpenUSD host. Local relative sublayers, references, payloads, authored
default variants, and texture dependencies are brokered by the viewer.

STEP/STP uses a third, dedicated zero-capability host with a pinned constrained
OCCT closure. It receives only an inherited read-only handle (never a path),
performs product-owned Part-21 admission before the CAD kernel runs, and emits
only normalized wire records.

Portable packaging keeps `Preview3D.exe` at the package root and the sandbox
executable plus its private DLL closure under `worker\`. The viewer prefers that
layout and grants the AppContainer read/execute only on `worker\`; same-directory
worker lookup remains as a developer/test-build fallback. The separate OpenUSD
bootstrap, core DLL, monolithic OpenUSD runtime, oneTBB, codec dependencies, and
hash-audited schema/plugin resources live only under `OpenUsdHost\`; its distinct
AppContainer is granted access only to that tree. The constrained OCCT closure
and `Preview3DStepHost.exe` live only under `StepHost\`, with a third distinct
AppContainer granted access only to that tree.

## Not included

Animation playback, editing, other CAD formats (IGES/IFC/JT/native CAD),
Explorer thumbnails (including for 3MF, USD, FBX, and STEP), network assets,
USD skeletal deformation, MaterialX, procedural schemas, and interactive variant
selection are not currently included. USD skeletal bindings are ignored and the
authored rest pose is previewed statically (matching the baked start pose FBX
already shows).

The supported STEP/STP subset is bounded and self-contained only: external STEP
documents are out of scope, and there is no PMI/GD&T, editing, saved views, exact
measurement, or shape healing.

3MF slicer-private multi-plate grouping, printer/process settings, Slice, Secure
Content, Volumetric, Implicit, toolpath, repair, slicing, and export features are
not supported.

FBX geometry caches, dynamic constraints, NURBS/subdivision tessellation,
cameras, and lights are outside the supported static subset. TGA/DDS/HDR,
animation playback, advanced material lobes, meshoptimizer-built LOD/hierarchies,
and the persistent derived cache remain deferred work.

## Tier B ceilings

3MF and USD are bounded Tier B paths: among their ceilings are 2 GiB per primary
source, 4 GiB aggregate local source/archive expansion, and 20 million triangles
or points. USD also caps at 50,000 nodes. The separate USD compatibility host has
a commit cap of the lower of 4 GiB or 35% of physical memory.

## 3MF placement notes

For preview compatibility, a mesh labeled Core object type `other` is shown if
it is referenced by the build, as some slicers produce this even though the 3MF
Core specification disallows it. Slicer plate grouping and print settings do not
affect the preview; a multi-plate project may show all root-build objects
together. The 3MF build placement is preserved, including models positioned
head-down for printing. To view one upright on the grid, select ground axis Z
and use the ground direction button to make negative Z point up.

## Release qualification

The implemented USD viewer path has an immutable corpus and a standalone
sanitizer fuzz-smoke lane. Final release qualification still requires the
recorded clean-machine, repeated performance/heartbeat, soak, and signed-build
gates; see [USD-009 verification](USD-009-VERIFICATION.md). Explorer USD
thumbnails are a separate follow-up and are not installed.

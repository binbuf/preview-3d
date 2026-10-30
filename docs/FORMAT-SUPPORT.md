# Format support and limits

Preview 3D is a static, read-only viewer. This document describes the exact
supported subset, isolation model, and known limitations for each format.
For install and build instructions, see the [README](../README.md).

## Support matrix

| Format | Support |
| --- | --- |
| glTF 2.0 | `.glb` and `.gltf`, including local relative binary and image sidecars, bounded Draco/meshopt geometry, mesh quantization, KTX2/Basis, PNG/JPEG/WebP/TGA/DDS/HDR/EXR textures, material texture slots, texture transforms, `KHR_materials_pbrSpecularGlossiness` approximated as diffuse albedo plus a dielectric response, and `KHR_materials_transmission`/`KHR_materials_ior` approximated as Fresnel-weighted glass (an authored index of refraction at or below 1 keeps the surface opaque, since there is no optical interface). Visual/scene extensions that are parsed but not rendered (`KHR_materials_clearcoat`, `sheen`, `specular`, `iridescence`, `anisotropy`, `volume`, `dispersion`, `diffuse_transmission`, `emissive_strength`, `KHR_materials_variants`, `KHR_lights_punctual`) open with core PBR and a specific compatibility warning. A truly unknown required extension keeps the core scene and is reported as degraded. Core geometry wins: only a required geometry/decoding extension that cannot be decoded (`KHR_draco_mesh_compression`, `EXT_meshopt_compression`, `KHR_mesh_quantization`, `KHR_implicit_shapes`, `KHR_accessor_float64`) fails the import, and a missing optional/required texture never removes usable geometry. `MSFT_texture_dds` alternate image indexing is not enabled; DDS files are decoded as direct image sidecars. |
| Wavefront OBJ | `.obj` with optional local `.mtl` and texture sidecars; ufbx polygon triangulation, smoothing/generated normals, UVs, vertex colors, object/group meshes, MTL material factors, and broker-approved base-color, normal/bump, and emissive maps (PNG/JPEG/BMP/TIFF/WebP/KTX2/TGA/DDS/HDR/EXR) |
| FBX | Binary or ASCII `.fbx`, including static hierarchy, instances, supported materials/textures, and a deterministic baked start pose; supported skin and blend deformation is baked. Image references that carry the authoring machine's absolute path are reduced to their file name and resolve through the package lookup below |
| STL | ASCII and binary |
| PLY | ASCII and binary triangle meshes and point clouds |
| 3MF | The supported static `.3mf` preview subset: Core geometry/components/build items, Materials and Properties colors/textures, Production model parts, and bounded Beam Lattice previews. Slicer-private required namespaces that do not describe the mesh are ignored with a compatibility record; a required Slice/Volumetric/Implicit/Secure Content extension still fails typed because the visible geometry depends on it |
| Universal Scene Description | `.usd`, `.usda`, `.usdc`, and `.usdz`; static meshes, procedural `Sphere`/`Cube`/`Cylinder`/`Cone`/`Capsule`, `Points` as colored point splats, hierarchy/instances, common primvars, display color/opacity, creased or subdivided meshes as a control cage with a compatibility warning, bounded USD Preview Surface materials/textures, and bounded local composition |
| STEP | `.step` and `.stp`: ISO 10303-21 AP203/AP214/AP242 B-rep or authored AP242 tessellated geometry, assemblies/reused definitions, instance/shape/face colors, tolerant authored length units, and Draft/Balanced/Full import quality through a dedicated isolated OCCT host |
| IGES | `.igs` and `.iges`: bounded fixed-column S/G/D/P/T record streams, analytic B-rep surfaces, assemblies/subfigures where representable, and IGES colors through the same dedicated OCCT host. The optional global unit flag converts to the verified OCCT system millimetre; a missing flag assumes millimetres with a warning |
| BREP | `.brep`: the OCCT ASCII `CASCADE Topology` shape-set serialization. Topology and geometry only (no authored colors or units); the scene reports `UpAxisId::Unknown` and zero metres-per-unit, and the bounded shape/location table is admitted before `BRepTools::Read` |
| XBF | `.xbf`: the OCCT binary XCAF document (`BINFILE` + supported format version). Colors, assembly structure and placements are preserved; the XCAF length unit is reported when present (otherwise the documented OCCT millimetre default with a warning), and the scene up axis stays `Unknown` |
| FreeCAD Standard | `.fcstd`: the common single-part/PartDesign case. A bounded ZIP preflight requires `Document.xml` and at least one evaluated `*.brp` Part shape, rejects encryption/multi-disk/data descriptors/overlapping or over-expanding entries, and parses `Document.xml`/`GuiDocument.xml` with node/depth/size limits. Shape colors, transparency, visibility, local links and nested placements are applied; external links and unsupported FreeCAD features are skipped with warnings |
| COLLADA | `.dae`: static scene meshes, node transforms, normals/UVs/colors, diffuse/metallic/roughness/emissive factors, and embedded or approved local textures. No animation, cameras or lights |
| 3DS | `.3ds`: static triangle meshes, transforms, material groups/colors, and approved local textures. No animation, cameras or lights |
| DXF | `.dxf`: Assimp's limited 3DFACE and polyline subset only, not full DXF entities/blocks |
| DirectX | `.x`: static text/binary mesh geometry and materials where Assimp supports them; no animation or DirectX effect state |
| OFF | `.off`: static ASCII triangle/polygon meshes, triangulated on import |
| AMF | `.amf`: static mesh volumes and materials; no slicing or AMF production/print semantics |
| X3D | `.x3d`: static IndexedFaceSet geometry, materials/colors and approved local textures; no scripting, sensors or animation |

`.mtl` is always a sidecar and is never a primary open type. The Open dialog,
command line, drag/drop, single-instance activation, Retry, and Open With all
accept the same direct formats, case-insensitively.

## Orientation, units, and presentation defaults

The format catalog declares each family's source up axis, unit policy and
presentation profile; opening a file applies that profile once to view state
(lighting, ground grid, backdrop). A toolbar lighting or grid change wins for
the rest of the session.

| Family | Source up axis | Units | Opening preset |
| --- | --- | --- | --- |
| glTF / GLB | Y (mandated) | metres (mandated) | Studio PBR |
| FBX | Y after adapter normalization | 1.0 after adapter normalization | Studio PBR |
| USD / USDA / USDC / USDZ | authored (Y/Z/X) | authored metres-per-unit | Studio PBR |
| OBJ | Y (ecosystem default; override in the toolbar) | unspecified | Studio PBR |
| PLY mesh | unspecified | unspecified | Studio PBR |
| PLY point-only / 3DGS | unspecified | unspecified | Point cloud (unlit round colored splats, no grid) |
| STL / 3MF | Z | unspecified (STL) / millimetres (3MF) | Studio CAD |
| STEP | unspecified (STEP defines no display-up axis) | authored, verified positive metre factor; absent units assume millimetres and contradictory units use the first/dominant factor, both warned | Studio CAD |
| IGES | unspecified (IGES defines no display-up axis) | verified positive post-transfer metre factor (OCCT system millimetre); an absent global unit flag assumes millimetres and warns | Studio CAD |
| BREP | unspecified (no display-up axis) | unspecified (`0.0`, no authored unit); never guessed as millimetres | Studio CAD |
| XBF | unspecified (no display-up axis) | XCAF document length unit converted to a positive metre factor; absent metadata assumes the OCCT millimetre default with a warning | Studio CAD |
| FCStd | unspecified (no display-up axis) | millimetres (the FreeCAD Part payload convention); reported as `0.001` with a compatibility note | Studio CAD |
| DAE / X3D / DirectX / OFF | Y | unspecified (`0.0`); the exporter family authors no verifiable unit | Studio PBR for DAE/X3D/DirectX; Studio CAD for OFF |
| 3DS / DXF / AMF | Z | unspecified (`0.0`); the exporter family authors no verifiable unit | Studio CAD |

Notes:

- OBJ has no standard up axis. Preview 3D reports Y-up so ordinary exports
  appear upright; the ground-axis toolbar control still cycles X/Y/Z and the
  "show original orientation" setting bypasses the correction.
- FBX reads the authored axes/unit scale exposed by ufbx, but the wire contract
  delivers normalized Y-up/metres geometry, so the info panel reports Y and
  metres. An authored unit is never guessed or treated as metres; the
  ground-axis control is the user override for orientation.
- Assimp-backed formats (DAE, 3DS, DXF, X, OFF, AMF, X3D) are compatibility
  importers, not native paths: static triangles/materials only, no animation,
  cameras, lights, plug-ins, file writes or external path access. DXF support is
  limited to Assimp's 3DFACE/polyline entities and is not a full DXF importer.
  Line/point-only content is skipped with a warning; a file with no triangles
  fails typed.
- PLY point detection is content-based: a file that emits points and no
  triangles selects the point preset; a PLY with any triangle mesh keeps the
  PBR review preset.
- A CAD/point preset changes view state only. It never rewrites source
  materials and never overwrites a persisted user setting.

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
`.ktx2`, `.tga`, `.dds`, `.hdr`, `.exr`); a missing `.bin` buffer, `.mtl`, or USD layer is never resolved this
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

## TGA and DDS texture subsets

TGA and DDS are decoded by product-owned, bounded decoders in the import
worker; no DirectXTex or new third-party codec is linked.

- **TGA**: true-color image types 2 (uncompressed) and 10 (RLE), 24- or 32-bit,
  no color map, no interleaving, zero origin. Both left/right and top/bottom
  origin bits are honored. Rejected typed: paletted/indexed images, 15/16-bit
  or grayscale/color-mapped types, nonzero canvas origin, invalid alpha-bit
  descriptors, truncated/RLE-overrunning payloads, and dimensions or decoded
  bytes over budget. The extension is only a candidate filter; a `.tga` name
  with invalid header bytes falls back with a warning.
- **DDS**: legacy `DXT1` (BC1), `DXT5` (BC3), `ATI2`/`BC5U` (BC5), and DX10
  `BC1_UNORM`/`BC3_UNORM`/`BC5_UNORM`/`BC7_UNORM` including their `_SRGB`
  variants, all mapped to the existing closed `PixelFormatId` block set. Valid
  authored mip chains are preserved and can upload directly. Uncompressed
  24/32-bit RGB(A) legacy DDS and DXGI `R8G8B8A8_UNORM(_SRGB)` /
  `B8G8R8A8_UNORM(_SRGB)` decode to RGBA8 and regenerate mips. Rejected typed:
  cubemaps, texture arrays, volumes/3D, every other DXGI format, invalid header
  or pixel-format sizes, and payloads shorter than the declared block/level
  extent. A header that claims more mips than the bytes contain is clamped to
  the levels actually present.
- **ORM / ambient occlusion**: the metallic-roughness map's R channel is AO
  (common ORM packing), G is roughness, B is metallic. AO multiplies indirect
  diffuse and environment lighting only; direct-light specular is unchanged.
  The material wire record stays at 72 bytes, so no authored
  `occlusionStrength` is carried and R is applied at full strength.

## HDR and EXR texture subsets

Radiance HDR and OpenEXR are decoded only in the import worker; the viewer never
links a decoder. Both produce scene-linear float internally and convert to the
existing RGBA8 material contract on the CPU (no new `PixelFormatId`, so the
shared-section validator and GPU format table are unchanged).

- **Radiance HDR (`.hdr`)** — product-owned decoder. Accepts `#?RADIANCE` or
  `#?RGBE` with one `FORMAT=32-bit_rle_rgbe` header and a `-Y <h> +X <w>`
  resolution line (both sign bits are honored; XYZE and X-first layouts are
  rejected). New-style per-channel RLE and flat RGBE scanlines decode; old-style
  `0x01 0x01 0x01` RLE is not implemented. Header length, resolution, RLE packet
  bounds, encoded bytes, decoded bytes, pixels and `maxDimension` are checked
  before/while allocating.
- **OpenEXR (`.exr`)** — pinned tinyexr (BSD-3-Clause, vendored from the
  TinyUSDZ closure) behind a product preflight. Only single-part, non-tiled,
  non-deep scanline images are accepted. Compression is limited to NONE, RLE,
  ZIPS, ZIP and PIZ; B44/B44A/DWAA/DWAB/ZFP and unknown values are rejected
  before the decoder allocates. Channels must be HALF or FLOAT with x/y sampling
  1 (UINT rejected). R/G/B/A and Y channel names work, including layered
  `layer.R` names, with a first-three/single-channel fallback. Tile, deep,
  multipart and data-window/channel/pixel/byte over-budget files soft-fail.
- **Color policy** — the source is linear. Color and emissive slots use a
  deterministic Reinhard curve (`c / (1 + c)`, exposure 1.0) then the slot's
  sRGB transfer; normal and data slots are clamped only and stay linear. An
  EXR normal/roughness map is therefore used, not skipped. Over-budget or
  malformed files fall back with one texture warning and never fail the model.
- **Environment hook** — `LoadEnvironmentAsset` in the import worker returns a
  bounded linear equirectangular RGBA-float image (not a `Material` image
  chunk) from HDR or EXR bytes. It takes bytes only, never a path/URI, and uses
  the same encoded/decoded/pixel/dimension budgets. T14's procedural Studio
  environment needs no decode; an embedded HDRI would use this hook.

## Isolation model

All general parsing and decoding runs in `Preview3DImportWorker.exe` under a
zero-capability AppContainer, and models reach the viewer as validated
wire-format chunks. Local relative sidecars are brokered by the viewer; remote
assets and arbitrary resolvers/plugins are never allowed.

USD files first use TinyUSDZ in the general isolated importer. Stages requiring
supported composition, procedural Gprims (`Sphere`, `Cube`, `Cylinder`, `Cone`,
`Capsule`), `Points`, or authored creases/subdivision are retried atomically in
a separately isolated, lazily started OpenUSD host. The retry classification
(`UnsupportedComposition`) is only accepted before any fast-path batch has been
published, so the two producers never mix. Local relative sublayers, references,
payloads, authored default variants, and texture dependencies are brokered by
the viewer.

The OpenUSD host tessellates procedural primitives deterministically: 24x16
longitude/latitude for spheres, 24 radial segments with caps for
cylinders/cones, 24x8 per hemisphere plus the spine for capsules, and the
authored 12-triangle cube. Cylinder/cone/capsule honor the authored `axis`
(default Z) and X/Y/Z are all supported; `radius`, `height`, and `size` are
honored, while `extent` is treated only as a hint because bounds are computed
from the generated vertices. Creases and non-`none` subdivision schemes render
the authored control cage with a `Degraded` warning (no OpenSubdiv, no smoothing
level option). `Points` become a `PointList` chunk carrying display color,
display opacity, transforms and bounds; authored `widths` are ignored in favor
of the viewer's existing bounded point-splat size policy. Curves/NURBS remain
unsupported until line topology work.

STEP/STP, IGES, BREP, XBF and FCStd use the same dedicated zero-capability host
(`Preview3DStepHost.exe`) with a pinned constrained OCCT closure. It receives
only an inherited read-only handle (never a path), performs product-owned
Part-21 admission for STEP and format-specific record/shape-set admission for
IGES/BREP before the CAD kernel runs, and emits only normalized wire records.
IGES is read by feeding the admitted fixed-column records to OCCT's pinned
record parser in memory; no temporary file or path is created.

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

Animation playback, editing, other CAD formats (IFC/JT/Parasolid/DWG/native
CAD), Explorer thumbnails (including for 3MF, USD, FBX, STEP, IGES, BREP, XBF,
and FCStd),
network assets,
USD skeletal deformation, MaterialX, procedural schemas beyond the five
supported Gprims plus `Points`, curves/NURBS, and interactive variant selection
are not currently included. USD skeletal bindings are ignored and the authored
rest pose is previewed statically (matching the baked start pose FBX already
shows).

The supported STEP/STP subset is bounded. External STEP document declarations
are unresolved metadata rather than a veto: the isolated host never opens the
referenced path, keeps the usable main-document geometry, and reports a
compatibility warning. Missing units assume millimetres and contradictory units
use the reader's first/dominant factor, both warned. Draft/Balanced/Full import
quality selects a versioned deflection profile (Balanced is the default), and a
bounded, deterministic `ShapeFix` repair runs only after a definition fails to
tessellate. There is no PMI/GD&T, editing, saved views, exact measurement, or
general-purpose shape healing.

IGES is admitted only as a complete fixed 80-column S/G/D/P/T record stream with
bounded record/entity/reference counts; truncated, control-byte, or out-of-order
input fails typed before OCCT. Colors follow the IGES color entities that the
XDE reader can represent; assembly/subfigure structure is preserved where
present. BREP is admitted only as a `CASCADE Topology` V1-V3 shape set with a
bounded `TShapes`/`Locations` table; it has no colors and no authored units.
All CAD interchange formats reuse the STEP tessellation profile and
per-definition failure isolation.

XBF is admitted by its fixed `BINFILE` magic and supported binary-document
version, then opened with OCCT's BinXCAF drivers through the mapped handle; no
path is opened. FCStd is a ZIP package interpreted without extraction to disk:
the structure scan rejects encryption, multi-disk archives, streaming data
descriptors, unsafe/duplicate names, overlapping entries, per-entry sizes over
256 MiB, aggregate expansion over 512 MiB, and expansions over 200:1 before any
FreeCAD data is decoded. `Document.xml` is parsed with 8 MiB / 200,000-node /
64-depth limits and external or absolute shape paths are rejected. Only the
evaluated Part/PartDesign `*.brp` payload is rendered; sketches, history,
expressions, assemblies beyond nested placements, FEM/CAM and FreeCAD macros or
Python are not evaluated. External links and unknown objects are skipped with
bounded warnings rather than failing the local geometry.

3MF slicer-private multi-plate grouping, printer/process settings, Slice, Secure
Content, Volumetric, Implicit, toolpath, repair, slicing, and export features are
not rendered. A required extension that merely annotates the package is ignored
with an `Ignored` compatibility record; a required Slice, Volumetric, Implicit
or Secure Content namespace is terminal (`UnsupportedRequiredFeature`) because
the Core mesh/build data is not authoritative for the visible geometry. Missing
or malformed optional metadata namespaces, including PrusaSlicer/Bambu
Studio/OrcaSlicer/Cura private namespaces, never fail an otherwise valid Core
model.

FBX geometry caches, dynamic constraints, NURBS/subdivision tessellation,
cameras, and lights are outside the supported static subset. Animation playback,
advanced material lobes, meshoptimizer-built LOD/hierarchies, and the persistent
derived cache remain deferred work.

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

Per-corner material variation is bounded: when a triangle's corners disagree
about texture/sampler/texture-layer/mix, the adapter fans it from its centroid
into one sub-triangle per corner so each piece keeps one material, provided the
whole generation stays within the triangle and material ceilings. If a split
would exceed those ceilings, or a corner-only texture cannot be decoded, the
dominant corner is used for the whole triangle. Both choices are `Degraded`
compatibility records (`1010` split, `1011` dominant), never
`ResourceLimit`, and positions, bounds and corner UVs stay unchanged.

## Release qualification

The implemented USD viewer path has an immutable corpus and a standalone
sanitizer fuzz-smoke lane. Final release qualification still requires the
recorded clean-machine, repeated performance/heartbeat, soak, and signed-build
gates; see [USD-009 verification](legacy/USD-009-VERIFICATION.md). Explorer USD
thumbnails are a separate follow-up and are not installed.

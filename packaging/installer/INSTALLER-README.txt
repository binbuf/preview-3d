Preview 3D @VERSION@ for Windows 11 x64
========================================

Preview 3D is a local, read-only viewer for these direct-open types:

* .glb and .gltf 2.0, including broker-approved local relative sidecars;
* .obj polygon meshes with optional local .mtl and texture sidecars;
* binary or ASCII .fbx with static hierarchy/instances, supported materials and
  textures, and a deterministic baked start pose;
* ASCII or binary .stl;
* ASCII or binary little- or big-endian .ply triangle meshes and point clouds;
* the supported static .3mf preview subset: Core geometry, components, all root
  build items, Materials and Properties colors/textures, Production model
  parts, and bounded Beam Lattice previews; and
* .usd, .usda, .usdc, and .usdz static stages with meshes, hierarchy,
  instances/point instances, common primvars, display color, supported USD
  Preview Surface materials/textures, and bounded local composition; and
* self-contained .step and .stp ISO 10303-21 files (AP203/AP214/AP242) with
  bounded B-rep or authored AP242 tessellated geometry, assemblies and reused
  definitions, colors, and authored length units.

Other CAD formats (IGES/IFC/JT/native CAD), Explorer thumbnails (including for
3MF, USD, FBX, and STEP), editing,
animation playback, network assets, and a persistent model-derived cache are
not part of this release. 3MF slicer-private multi-plate grouping/settings,
Slice, Secure Content, Volumetric, Implicit, toolpath, repair, slicing, and
export features are not supported; standard root-build items are displayed
together in authored coordinates. FBX geometry caches, dynamic constraints,
NURBS/subdivision tessellation, cameras, and lights are outside its static subset.
USD skeletal data, MaterialX, procedural schemas, arbitrary plugins, remote
assets, and interactive variant selection are also outside the supported subset.

File associations
-----------------

Setup registers Preview 3D with Windows Default Apps and Open With for .glb,
.gltf, .obj, .fbx, .stl, .ply, .3mf, .usd, .usda, .usdc, .usdz, .step, and .stp.
Windows 11
requires the signed-in user to confirm default app choices. Setup offers to
open Preview 3D's Default Apps page after install;
select Preview 3D for each listed extension there. Existing user choices are
never overwritten by setup.

Isolation and data
------------------

Model parsing and decode run in the zero-capability
Binbuf.Preview3D.ImportWorker AppContainer. The worker has read/execute access
only to its private worker payload: setup provisions that exact deterministic
package SID on the protected worker directory, and each user creates or opens
the corresponding profile on first import. Model and sidecar bytes are supplied
through the viewer's bounded broker. The product does not upload or modify models.
USD first runs through TinyUSDZ in that worker. Supported local composition
falls back atomically to Binbuf.Preview3D.ImportHost, a separate zero-capability
AppContainer that can read/execute only the private OpenUsdHost payload and exits
after the generation. Local relative stage/image dependencies are supplied as
brokered bytes; remote assets and model-selected resolvers/plugins are rejected.
USD follows Tier B ceilings (including 2 GiB primary, 4 GiB aggregate local
bytes/USDZ expansion, and 20 million triangles or points); the compatibility
host's commit cap is min(4 GiB, 35% of physical memory). STEP/STP runs in
Binbuf.Preview3D.StepHost, a third zero-capability AppContainer that can
read/execute only the private StepHost CAD-kernel payload, receives the source
as an inherited read-only handle rather than a path, and exits after the
generation. Its commit cap is also min(4 GiB, 35% of physical memory). No
importer can read another's private payload directory.

Preview 3D stores small UI preferences under
%LOCALAPPDATA%\Binbuf\Preview 3D. Uninstall leaves those preferences in place
and attempts to remove the uninstalling user's AppContainer profile. It removes
only product-owned registration; source models and unrelated file associations
are not touched.

Runtime and release metadata
----------------------------

The installation contains its app-local MSVC runtime, worker dependencies, and
the exact private OpenUSD DLL/resource tree and OCCT STEP-host DLL tree.
Direct3D 12 feature level 11_0 or later is required. MANIFEST.json records
payload SHA-256 values; SBOM.cdx.json, THIRD-PARTY-NOTICES.txt, and licenses\
record dependency provenance and redistribution notices.

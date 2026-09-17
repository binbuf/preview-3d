3D Preview @VERSION@ for Windows 11 x64
========================================

3D Preview is a local, read-only viewer for these direct-open types:

* .glb and .gltf 2.0, including broker-approved local relative sidecars;
* .obj polygon meshes with optional local .mtl and texture sidecars;
* binary or ASCII .fbx with static hierarchy/instances, supported materials and
  textures, and a deterministic baked start pose;
* ASCII or binary .stl; and
* ASCII or binary little- or big-endian .ply triangle meshes and point clouds.

3MF, USD, CAD formats, Explorer thumbnails (including for FBX), editing,
animation playback, network assets, and a persistent model-derived cache are
not part of this release. FBX geometry caches, dynamic constraints,
NURBS/subdivision tessellation, cameras, and lights are outside its static subset.

File associations
-----------------

Setup registers 3D Preview with Windows Default Apps and Open With for .glb,
.gltf, .obj, .fbx, .stl, and .ply. Windows 11 requires the signed-in user to confirm default
app choices. Setup offers to open 3D Preview's Default Apps page after install;
select 3D Preview for each listed extension there. Existing user choices are
never overwritten by setup.

Isolation and data
------------------

Model parsing and decode run in the zero-capability
Binbuf.Preview3D.ImportWorker AppContainer. The worker has read/execute access
only to its private worker payload: setup provisions that exact deterministic
package SID on the protected worker directory, and each user creates or opens
the corresponding profile on first import. Model and sidecar bytes are supplied
through the viewer's bounded broker. The product does not upload or modify models.

3D Preview stores small UI preferences under
%LOCALAPPDATA%\Binbuf\3D Preview. Uninstall leaves those preferences in place
and attempts to remove the uninstalling user's AppContainer profile. It removes
only product-owned registration; source models and unrelated file associations
are not touched.

Runtime and release metadata
----------------------------

The installation contains its app-local MSVC runtime and worker dependencies.
Direct3D 12 feature level 11_0 or later is required. MANIFEST.json records
payload SHA-256 values; SBOM.cdx.json, THIRD-PARTY-NOTICES.txt, and licenses\
record dependency provenance and redistribution notices.

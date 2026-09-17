Preview3D @VERSION@ portable engineering package (Windows 11 x64)
================================================================

Run Preview3D.exe and use Ctrl+O, or pass one local model path on the command
line. Keep the worker directory beside Preview3D.exe. The app does not require
administrator rights, write file associations, or modify the opened model.

Supported content
-----------------

* GLB and glTF 2.0 with local relative binary/image sidecars.
* OBJ polygon meshes with optional local MTL and texture sidecars.
* ASCII and binary STL.
* ASCII and binary little- or big-endian PLY triangle meshes and point clouds.
* The bounded glTF subset includes static meshes/instances, vertex colors,
  metallic/roughness materials, normal/emissive textures, unlit materials,
  alpha modes, KHR_texture_transform, KHR_draco_mesh_compression,
  KHR_mesh_quantization, EXT_meshopt_compression, KHR_texture_basisu/KTX2,
  and EXT_texture_webp.

Important limits
----------------

This is a local, read-only static viewer. Animation, editing, network assets,
USD, 3MF, FBX, CAD, thumbnails, file associations, and a
persistent derived cache are outside this limited MVP. Optional unsupported
glTF material/image features may fall back with a warning; required unsupported
extensions fail. OBJ supports faces, triangulation, smoothing/generated normals,
UVs, vertex colors, object/group meshes, MTL factors, and broker-approved
base-color, normal/bump, and emissive maps. Lines, curves, animation, and distinct
roughness/metalness texture maps are not rendered. Imports and decoded data are
bounded; over-limit or malformed models fail instead of rendering partially.

The worker runs in a zero-capability AppContainer. On first import, Preview3D
creates the current-user profile Binbuf.Preview3D.ImportWorker and grants that
profile read/execute access only to this package's worker directory. Model and
sidecar data are supplied as read-only handles by the viewer; the worker is not
granted access to the model directory. The profile creation and ACL update need
no elevation. Deleting the extracted directory removes the granted payload.

Cleanup
-------

Close Preview3D first. To also remove the per-user AppContainer profile and its
worker-directory ACL entry, run:

  powershell -NoProfile -ExecutionPolicy Bypass -File .\Remove-Preview3DProfile.ps1

No daemon, service, shell extension, registry file association, or cache is
installed by this package.

Runtime and support
-------------------

The archive contains the required app-local MSVC runtime and worker dependency
closure. Direct3D 12, DXGI, Direct2D, DirectWrite, WIC, and D3DCompiler 47 are
Windows 11 system components and are not redistributed. A Direct3D feature
level 11-capable adapter/driver is required. Coarse/full rendering intentionally
uses bounded representative geometry and view-driven refinement; it is not an
authoring-fidelity or complete-scene residency guarantee.

MANIFEST.json records every other payload's SHA-256. SBOM.cdx.json records
component versions and vcpkg provenance. THIRD-PARTY-NOTICES.txt and licenses\ contain
redistribution notices. The ZIP's adjacent .sha256 file verifies the archive.
An archive whose manifest says "signed": false is an engineering build and is
not a release candidate.

# USD-001 fixtures

`mesh.usda` is an original Preview3D test fixture authored for USD-001. It is
covered by the repository's license and exercises a mesh, face-varying normals
and UVs, display color, hierarchy, transform, units, and Z-up metadata.

`static-scene.usda` is an original Preview3D USD-004 policy fixture. It covers
X-up/millimetre metadata, explicit start-time evaluation, a large double
transform, vertex normals/UVs/colors, a shared point-instancer prototype,
per-instance scale/orientation/visibility, and guide-purpose omission.

`materials.usda` is an original Preview3D USD-005 fixture covering Preview
Surface factors, a brokered PNG, UV primvar selection and transform,
double-sided state, and a complete two-face `materialBind` partition.

`cube.usdc.base64` and `cube.usdz.base64` are byte-for-byte base64 encodings of
`models/cube.usdc` and `models/cube.usdz` from TinyUSDZ v0.9.1, immutable commit
`a04ee0bcbd1a930e30cc40938fcee3526a6fa8eb`. They are redistributed under
TinyUSDZ's Apache-2.0 license, copied here as `TinyUSDZ-LICENSE.txt`. The test
harness decodes them directly to memory; it never writes a model into the
worker sandbox.

The authoritative decoded-byte hashes are recorded in
`.docs/USD-001-SPIKE-RESULTS.md` and checked by the harness before parsing.

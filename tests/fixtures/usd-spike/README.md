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

`compat-composition.usda`, `compat-sub.usda`, `compat-ref.usda`, and
`compat-payload.usda` are original Preview3D USD-007 fixtures. Together they
cover brokered sublayers, references, payload loading, inherits, specializes,
and an authored default variant selection. The independent expected result is
six triangle geometries and six instances at Z-up / 0.01 metres per unit.
`compat-point-instancer.usda` is an original USD-007 fixture for bounded
OpenUSD point-instancer expansion and an authored invisible ID; its independent
expected result is one shared triangle geometry with two visible instances at
minimum X coordinates 0 and 4. These text fixtures are covered by the
repository's license. Their immutable SHA-256 hashes are:

| Fixture | SHA-256 |
| --- | --- |
| `compat-composition.usda` | `978d20fa211df079e1c58ec4b850371248157382ef419adf149328e20c10e7c3` |
| `compat-payload.usda` | `d4f2d1588218de42eec1dbbc1258fa1ce418f57652ca22116f7acd05e41482db` |
| `compat-point-instancer.usda` | `4f1b5ddcb318ec0e629d37c62bf7faff6ae341eaf07b483e34c0b2f403a2a70d` |
| `compat-ref.usda` | `ccd77a3bb27608639b1a178766a00c5905c1c7a89718deb67c7033d85874129a` |
| `compat-sub.usda` | `a11a3798ebabeeb81f7cbada0397bb7294e83ff71fa42173b48a405798884df4` |

`primitives.usda`, `primitivesZ.usda`, `usd_points_rgb.usda`,
`subdiv_cube_cc.usda` and `mixed_mesh_primitives.usda` are original Preview3D
USD-T11 fixtures covered by the repository's license. They cover the five
procedural Gprims with authored X/Y/Z and default-Z axes, per-point
displayColor/displayOpacity with ignored widths, a creased Catmull-Clark control
cage, and a mixed mesh+primitive scene. Their immutable SHA-256 hashes are:

| Fixture | SHA-256 |
| --- | --- |
| `primitives.usda` | `86aff0e47d2af7cd26e7d5b215896818b4ac3d82e4e3c0f462d6023c6e31dc2e` |
| `primitivesZ.usda` | `17f3b2764b9e3908aa28c4dbf7a18fc211b8eeccadf1f1ab1bfe0b3bc6c2173b` |
| `usd_points_rgb.usda` | `37be4445b834d92cbe6c78ce7b11a674515ab3d41f31bb25ef7af56a00387418` |
| `subdiv_cube_cc.usda` | `64b80ce1b04b56b97fedc28ef22fdb22a1e2f90b124d6a8290ea71d16583e50f` |
| `mixed_mesh_primitives.usda` | `81a307d8dc80e57129730a182eb02295b1a6096f189666511fd7c7d8b388194c` |

`cube.usdc.base64` and `cube.usdz.base64` are byte-for-byte base64 encodings of
`models/cube.usdc` and `models/cube.usdz` from TinyUSDZ v0.9.1, immutable commit
`a04ee0bcbd1a930e30cc40938fcee3526a6fa8eb`. They are redistributed under
TinyUSDZ's Apache-2.0 license, copied here as `TinyUSDZ-LICENSE.txt`. The test
harness decodes them directly to memory; it never writes a model into the
worker sandbox.

The authoritative decoded-byte hashes are recorded in
`.docs/USD-001-SPIKE-RESULTS.md` and checked by the harness before parsing.

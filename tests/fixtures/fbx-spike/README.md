# FBX-001 spike fixtures

These fixtures are test-only inputs for the non-product FBX spike. They do
not enable `.fbx` in the viewer, broker, worker protocol, installer, or Shell
surfaces.

The authored FBX fixture content was copied from the `data/` directory of
`ufbx` 0.23.0 (`ufbx` commit/tag `v0.23.0`), the exact version pinned by this
repository's vcpkg baseline. Text fixtures have one terminal LF added by the
repository patch format; binary fixtures decode byte-for-byte to the upstream
files. The upstream repository offers
the software and its test data under either the MIT license or the Unlicense;
this repository uses the MIT option. See `UFbx-LICENSE.txt` in this directory.

| Local fixture | Upstream fixture | Purpose |
| --- | --- | --- |
| `hierarchy-instances-pivots-ascii.fbx` | `maya_instanced_pivots_7700_ascii.fbx` | ASCII parsing, hierarchy, pivots, and repeated mesh instances |
| `dual-quaternion-ascii.fbx` | `maya_dual_quaternion_7500_ascii.fbx` | Dual-quaternion skinning API and evaluated static pose |
| `multiple-stacks-ascii.fbx` | `synthetic_anim_stack_no_props_7100_ascii.fbx` | Observable authored animation-stack order |
| `cube-binary.fbx.base64` | `blender_272_cube_7400_binary.fbx` | Small binary FBX, stored as base64 so the textual fixture is reviewable |
| `shape-animation-binary.fbx.base64` | `blender440_shape_weight_anim_7400_binary.fbx` | Animated blend-shape evaluation |
| `linear-skin-binary.fbx.base64` | `blender_293_half_skinned_7400_binary.fbx` | Linear skinning and unweighted vertices |
| `nested-hierarchy-binary.fbx.base64` | `blender_279_nested_meshes_7400_binary.fbx` | Node-depth limit enforcement |
| `blended-skin-binary.fbx.base64` | `max_transformed_skin_7500_binary.fbx` | Blended dual-quaternion/linear skinning |
| `combined-skin-blend-ascii.fbx` | `maya_axes_anim_7700_ascii.fbx` | Combined skin-plus-blend evaluation and animated multi-axis transforms |
| `negative-scale-pivots-ascii.fbx` | `maya_pivot_offset_7700_ascii.fbx` | Authored negative scale with pivot transforms |
| `nonuniform-scale-pivots-ascii.fbx` | `maya_split_pivot_7700_ascii.fbx` | Authored non-uniform scale with split pivots |
| `z-up-binary.fbx.base64` | `blender_340_z_up_7400_binary.fbx` | Z-up source-axis conversion to the right-handed Y-up target |

The harness decodes `.fbx.base64` files in memory and passes the resulting
bytes directly to `ufbx_load_memory()`. SHA-256 provenance values are recorded
in `.docs/fbx/FBX-001-SPIKE-RESULTS.md`.

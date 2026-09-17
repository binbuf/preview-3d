# FBX-001 spike results

Status: complete — proceed to FBX-002
Measurement date: 2026-09-17
Pinned importer: `ufbx` 0.23.0

## Current decision

Proceed to FBX-002. The capped-memory, deterministic-pose/deformation, load
cancellation, and forced evaluation-cancellation paths pass in Debug and
Release. The documented Tier-B limits and pose rule are feasible; no design or
ADR revision is required.

The harness lives in `tests/import-isolation/FbxSpikeTests.cpp` and the worker's
explicit `--fbx-spike-pool` mode. The broker's `InitializeForTesting()` seam is
never selected by product callers. This work does not add an FBX worker opcode,
viewer route, extension filter, activation path, installer registration,
thumbnail path, or release claim.

## Build and hardware

| Item | Value |
| --- | --- |
| Source revision at measurement start | `bf7f15ae66dca3a0921e1b9b4494e6c1ba281701` plus this working-tree change |
| OS | Windows 11 Pro 10.0.26200 (build 26200) |
| CPU | AMD Ryzen 9 7900X3D, 12 cores / 24 logical processors |
| Physical memory | 67,810,054,144 bytes |
| Compiler/build | MSVC via MSBuild 18.10.1.42706, x64 Debug and Release |
| Dependency | vcpkg-pinned `ufbx` 0.23.0; compile-time version assertion in the harness |
| Test command | `Tests.ImportIsolation.exe "[fbx-spike]" --durations yes` |

The harness uses explicit 64 MiB temp and 64 MiB result limits for both load
and evaluation, plus a 1,000,000-allocation cap per allocator. These deliberately
tighter fixture caps validate the callbacks; they do not replace the product
Tier-B ceiling (1.5 GiB or 35% of physical memory, whichever is lower) or the
4 GiB worker Job limit.

## Fixture provenance

All authored inputs come from the `data/` corpus in the upstream `ufbx` 0.23.0
source tree and are redistributed under its MIT option. A license copy and the
upstream-to-local mapping are in `tests/fixtures/fbx-spike/README.md`. Binary
inputs are base64 text in Git and are decoded directly to memory by the harness.
Hashes below cover the exact bytes passed to `ufbx_load_memory()`.

| Fixture | Decoded bytes | SHA-256 |
| --- | ---: | --- |
| `blended-skin-binary.fbx.base64` | 49,424 | `BC2A6B22EB3AC80704E7484B853D24AA50CF5C1DE8AB7E9A225A837290A6A5A4` |
| `combined-skin-blend-ascii.fbx` | 138,332 | `C452FE966286DE6EC22F9334A8FF4FFC03D79AFE04F2D5181779A0D022B2BB4F` |
| `cube-binary.fbx.base64` | 11,020 | `BB78C7EE90FA811BAA9A4400B613C845D3A5329F4BFA87FF5E20692DDB8BD2AE` |
| `dual-quaternion-ascii.fbx` | 24,691 | `2A3FE6DD3AE1345538CFD4A944B3327F1D0C993E04BDD3214FEC2E7AC109CC1F` |
| `hierarchy-instances-pivots-ascii.fbx` | 15,301 | `5C5EE7D7BB4B39B23FEC8A120FFA42CBE5856B5C5B12B23EE957D55EB81E6D22` |
| `linear-skin-binary.fbx.base64` | 15,740 | `537700C737063BC9E96D3BB34DC7C7400F6327D7036FB24193E0028364337140` |
| `multiple-stacks-ascii.fbx` | 31,263 | `040312E87AF89C32D5EAE6015A7E0B8FE55CEFE7E8B69F104F299C0E86035F2D` |
| `negative-scale-pivots-ascii.fbx` | 57,957 | `0B17954C6113E9CE19FBBD51ADBC9353161EA5FF2C1F6C048E9C8280DF2A6FEC` |
| `nested-hierarchy-binary.fbx.base64` | 16,316 | `7ADAB361044B68170AAF3D460137EF004483FFD08C76EA73172CC449CB6348A9` |
| `nonuniform-scale-pivots-ascii.fbx` | 31,075 | `749EA04A78CB3C16A5A9E136C816046B3E550D9CBCC1686AFF52909E343BD425` |
| `shape-animation-binary.fbx.base64` | 27,500 | `F96F2D4888931414457A2020BD07E380EC9F4F47EC21C40E71A34F99CCC2DD99` |
| `z-up-binary.fbx.base64` | 28,860 | `6573C345BCF70DAD2F5D7B4A2CC3B64123DA93E5DEA938C2223607F661DDAF34` |

Malformed data is generated deterministically by truncating the small binary
cube at half length. The node-depth limit reuses the nested hierarchy fixture.
The excessive-count case expands one ASCII `Vertices` declaration to
12,000,000 real values (24 MB of generated source text) and is intentionally
generated at test time instead of committing a large redundant file.

## Measurements

The allocator peaks below come from separate tracking allocators supplied to
`ufbx_load_memory()` and `ufbx_evaluate_scene()`. “Normalized bytes” is the
peak size of the canonical spike snapshot (finite world positions, inverse-
transpose world normals, and double-precision instance transforms) and is
tracked separately from both ufbx allocator classes.

| Fixture | Load temp peak | Load result peak | Eval temp peak | Eval result peak | Normalized bytes | Quantized hash |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| hierarchy/instances/pivots ASCII | 136,728 | 65,536 | 0 | 19,040 | 1,344 | `8a53faf614cbd2ad` |
| cube binary, no animation stack | 109,448 | 32,768 | 0 | 12,288 | 672 | `751006bf2e6edb8a` |
| linear skin binary | 138,120 | 65,536 | 4,096 | 16,496 | 336 | `dd14510eabd5a5d8` |
| dual-quaternion skin ASCII | 177,688 | 131,072 | 4,768 | 38,944 | 4,272 | `3df7954d0ea5a3d1` |
| blended DQ/linear skin binary | 169,240 | 131,072 | 9,920 | 42,864 | 8,784 | `787dbfc522a4bd7e` |
| animated blend shape binary | 156,952 | 65,536 | 4,096 | 33,232 | 672 | `ac9676352d12e4be` |
| combined skin plus blend ASCII | 335,832 | 262,144 | 4,768 | 121,872 | 12,528 | `86c38e83d528b6d7` |

Eight load/evaluate/hash repetitions per deformation fixture were identical
within each test invocation. Three additional Debug and three Release process
invocations produced identical seven-record hash sets; Debug and Release hashes
also match. The tolerance is a fixed `1e-6` quantization after rejecting NaN,
infinity, and values outside the supported finite range.

Tiny temp peaks of zero are legitimate: undeformed evaluation did not request
temporary allocator storage for those fixtures. They are measurements, not an
assumption that all undeformed files need no temp memory.

## Decisions established so far

### Pose and evaluated attributes

- Preserve `scene.anim_stacks` authored order. If it is nonempty, use
  `anim_stacks.data[0]->anim` at `anim_stacks.data[0]->time_begin`. The corpus
  exposes `X`, `Y`, `Z` in that order and separately verifies a first-stack
  start greater than zero. Do not sort stacks by name or evaluate at zero.
- If there is no animation stack, evaluate `scene.anim` at time zero to obtain
  the default/rest scene. The binary cube covers this branch.
- Set `evaluate_skinning=true` and `evaluate_caches=false` for load and static
  evaluation. `mesh.skinned_position` and `mesh.skinned_normal` are the
  authoritative deformed attributes. When `mesh.skinned_is_local` is true,
  transform position by `node.geometry_to_world` and normal by
  `ufbx_matrix_for_normals(node.geometry_to_world)` before normalization.
- Blend evaluation is already reflected in the evaluated mesh attributes. The
  combined fixture proves that consuming `skinned_position` after one static
  scene evaluation includes both blend and skin deformation; applying blend
  offsets again would be incorrect.
- Honor `mesh.reversed_winding`. Mirrored/non-uniform transforms use the
  inverse-transpose normal matrix; the harness checks transformed tangent/
  normal orthogonality and observes winding reversal through the pinned API.
- Normalize with `UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY`, right-handed Y-up,
  one unit per meter, retained pivots, preserved geometry transforms, and
  helper nodes for unsupported inherit modes. Authored negative and non-uniform
  scale fixtures remain observable; a centimeter source reports a `0.01`
  geometry scale, and a Z-up source reports the expected non-identity root
  rotation. This makes the adapter contract explicit and keeps hierarchy/
  instances observable.

### Sharing key

An undeformed mesh may share normalized GPU vertex/index buffers when the key
contains the source mesh identity, geometry-conversion options, generated-
attribute decisions, topology/part partition, and winding state. Node world
transforms and per-instance material bindings remain instance data and do not
force geometry duplication.

Evaluated geometry may share only after a bounded canonical comparison proves
the same mesh identity, selected pose, effective skin/blend result, winding,
generated attributes, and material-part topology. Per-instance bone results,
different deformer weights, geometry transforms baked into vertices, or a
different part topology require duplication. The production catalog must use
the existing 50,000-object and 32,768-material bounds; it must not create an
unbounded whole-scene hash table. FBX-002 will encode the bounded key and scene
instance contract.

### Unsupported-feature policy

The pinned API exposes geometry caches through `cache_deformers/cache_files`,
constraints through `constraints`, NURBS through `nurbs_curves`,
`nurbs_surfaces`, and trim objects, subdivision through per-mesh subdivision
levels/results, procedural geometry through `procedural_geometries`, and
layered/procedural/shader materials through `ufbx_texture.type`. Embedded media
is available through bounded `content` blobs; file textures expose original,
relative, and absolute reference strings.

Production decisions for the static preview subset are:

- geometry caches are never evaluated or requested: warn and omit them when
  ordinary visible polygons remain, otherwise return
  `UnsupportedRequiredFeature`;
- dynamic constraints are not retained: warn when the evaluated static result
  still has usable supported geometry, otherwise fail typed;
- NURBS, subdivision-only, and procedural geometry are not tessellated: warn
  when independent polygon geometry remains, otherwise fail typed;
- layered/procedural/shader textures use only an already-resolved supported
  file/PBR leaf when unambiguous, otherwise use the deterministic neutral
  material and warn; material failure does not hide valid geometry;
- embedded images are accepted only after encoded-byte, MIME/container,
  dimension, decoded-pixel, and aggregate limits; they use the existing worker
  image decoder path;
- external image references go only through the broker-approved local sibling
  handle path. Absolute, traversal, ADS, device, and network references fail
  closed. External geometry-cache references are never brokered.

## Passing evidence

- Debug build and tagged run: 448 assertions in 10 cases.
- Release build and tagged run: 448 assertions in 10 cases.
- Full Debug isolation suite: 52,612 assertions in 219 cases.
- Full Release isolation suite: 52,612 assertions in 219 cases.
- ASCII and binary `ufbx_load_memory()` paths pass with explicit allocators.
- Linear, dual-quaternion, blended DQ/linear, blend-only, and combined
  skin-plus-blend evaluated poses are finite and deterministic.
- A progress callback cancels load with `UFBX_ERROR_CANCELLED`.
- Separate 1 KiB temp and result caps fail with controlled ufbx limit errors;
  the next ordinary load/evaluation succeeds.
- Binary truncation and a depth limit fail cleanly; the next valid load succeeds.
- A generated 12-million-real vertex array exhausts the explicit allocator cap
  with a controlled limit error, stays below both caps, and does not poison the
  following valid load.
- Repeated mesh instances retain one `ufbx_mesh` relationship while their node
  transforms remain distinct.
- The zero-capability AppContainer worker is observed inside
  `ufbx_evaluate_scene()`, misses the full cooperative grace, is terminated by
  the Job Object, and is replaced with a different PID in the same pool slot.
  The replacement immediately completes a valid generation and a subsequent
  real glTF import also succeeds.

## Evaluation cancellation observation

`ufbx_evaluate_scene()` has no progress callback in 0.23.0. The test worker
therefore uses a deterministic five-second delay in its first evaluation
allocator callback and publishes a shared state only after execution is inside
the ufbx call. The host duplicates and signals the same manual-reset event type
used by product cancellation. It receives no cooperative reply for 500 ms,
then calls the real `WorkerPool::TerminateAndReplace()` path.

Across three Debug and three Release repetitions, cancel-to-replacement time
was 503–517 ms; the largest observed across all focused/tagged runs was 518 ms.
Without termination the deliberately non-interruptible callback would remain
inside evaluation for five seconds. The measured result confirms that the
500 ms broker grace, not ufbx evaluation duration, bounds cancellation and
that replacement does not poison the pool. This is containment evidence, not
a claim that ordinary evaluation itself takes 500 ms.

## Proceed rationale

FBX-002 may use the pose, evaluated-attribute, sharing-key, feature-policy, and
allocator decisions above. Later product tasks must retain all Tier-B count and
byte checks in addition to ufbx allocator limits, and must route cancellation
through the existing worker event plus 500 ms terminate/replace backstop.

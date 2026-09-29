# FBX-004: deterministic static deformation pose

Status: complete (2026-09-17)
Depends on: FBX-001, FBX-003
Unblocks: FBX-005

## Objective

Evaluate the documented deterministic FBX pose and bake supported skin and
blend deformation into validated static geometry without retaining animation
curves or adding playback.

## Context to load

- FBX-001 results and fixtures
- `import-worker/src/FbxAdapter.{h,cpp}`
- pinned `ufbx.h` structures for scenes, animation stacks, evaluated scenes,
  `skinned_position`, `skinned_normal`, skin deformers, and blend deformers
- `shared/model-core/include/model_core/WireFormat.h`
- `shared/import-broker/src/ImportSession.cpp`
- `tests/import-isolation/FbxImportTests.cpp`

## Work

1. Select the first authored animation stack and evaluate at its authored start
   time. With no stack, use the file's default/rest evaluation. Record animation,
   skin, and bone counts in `SceneMetadata`; do not serialize names or curves.
2. Run `ufbx_evaluate_scene()` with explicit temp/result allocator limits,
   skinning enabled, geometry caches and external files disabled, and the exact
   flags decided by FBX-001. Map library allocation/format errors to the product
   taxonomy without allowing exceptions across the adapter boundary.
3. Normalize evaluated/skinned positions and normals, including blend-channel
   weights, combined skin-plus-blend results, inverse-transpose normals,
   mirrored winding, and finite checks. Generate normals/tangents only when the
   evaluated source does not provide usable values and the material/layout needs
   them.
4. Reuse geometry only under the bounded equivalence key approved by FBX-001.
   Two instances with different evaluated deformation must not alias one GPU
   buffer; identical evaluated results may share. Keep per-instance material
   and transform bindings independent.
5. Detect geometry caches, dynamic constraints, NURBS, subdivision surfaces,
   and unsupported deformer modes. Emit a bounded warning when omission leaves
   coherent supported visible geometry; return `UnsupportedRequiredFeature`
   when it removes required/all visible geometry. Never silently render a
   plausible but wrong rest pose for a required unsupported deformation.
6. Add cancellation checkpoints immediately before/after library evaluation and
   throughout normalization. If cancellation occurs inside the uninterruptible
   library call, rely on the already-proven broker grace/worker replacement;
   never lengthen the UI wait or reuse that worker slot.
7. Add numeric golden tests for each deformation mode, stack selection and
   non-zero start time, rest fallback, deformed instance sharing/non-sharing,
   warnings/failures, evaluator allocator exhaustion, cancellation, and valid
   reopen after forced termination.

## Constraints

- No animation playback, animation UI, per-frame skinning, GPU skin buffers, or
  retained curves.
- Do not enable geometry-cache file requests.
- Golden expectations must be independent numeric results, not values generated
  at test time by the same `ufbx` call being tested.

## Verification

- Binary and ASCII golden scenes match expected posed vertices, normals,
  bounds, transforms, counts, and warnings within documented tolerances.
- First-stack start selection is deterministic across repeated Debug/Release
  runs and multiple worker processes.
- Unsupported required deformation fails rather than displaying rest geometry.
- Cancellation/limit termination leaves UI/import broker responsive and the
  next valid import succeeds.
- Full Unit and ImportIsolation suites, including hostile-worker tests, pass.

## Completion record

- The product adapter now loads animation/deformer data, selects the first
  authored stack without sorting and evaluates its authored `time_begin`; a
  scene with no stack evaluates `scene.anim` at zero. Both load and evaluation
  retain explicit split temp/result memory and allocation limits, disable cache
  and external-file evaluation, and use the FBX-001 option set. Animation,
  skin, and bone counts cross in `SceneMetadata`; curves and names do not.
- Evaluated `skinned_position`/`skinned_normal` values are authoritative for
  linear, rigid, dual-quaternion, blended DQ/linear, blend-only, and combined
  skin-plus-blend results. Local values use `geometry_to_node`; world-space
  deformed values are converted back through inverse `node_to_world` before
  serialization so the retained node transform is not applied twice. Normals
  use the corresponding inverse transpose, unusable normals receive a bounded
  face fallback, mirrored winding is honored, and required tangents are
  regenerated from the evaluated triangle.
- Geometry reuse is keyed by source mesh identity, topology/attribute choices,
  winding, and a canonical evaluated node-local position/normal fingerprint,
  followed by exact comparison. Hashing/comparison work is charged against the
  Tier-B index ceiling, so hostile instance catalogs cannot create unbounded
  equivalence work. Equivalent deformed instances share; translated instances
  whose bone result differs in node-local space split.
- Cache-deformed and subdivision meshes are omitted instead of displaying a
  plausible rest cage. Caches, constraints, NURBS/trim objects, subdivision,
  and procedural geometry produce bounded feature warnings when independent
  visible polygon geometry remains; a NURBS-only fixture fails with
  `UnsupportedRequiredFeature`. Geometry-cache files are never requested.
- Cancellation is checked before/after the uninterruptible evaluator and every
  1,024 evaluated indices during comparison, in addition to the existing face,
  node, instance, batch, and load-progress checkpoints. A test-only 1 KiB
  evaluation-allocator request proves `ScratchLimit` and same-worker recovery;
  FBX-001's real five-second allocator stall continues to prove the broker's
  500 ms terminate/replace backstop for cancellation inside evaluation.
- Numeric 1e-6 wire goldens cover binary/ASCII linear, dual-quaternion,
  blended, blend-only, combined deformation, multiple authored stacks, a
  non-zero stack start, rest fallback, transforms, normals, tangents, bounds,
  counts, warnings, repeated determinism, and Debug/Release equivalence.
  Debug and Release focused FBX tests pass 47,670 assertions in 13 cases. Full
  Debug/Release Unit suites pass 7,609/7,521 assertions in 98 cases; full
  ImportIsolation suites pass 100,395 assertions in 237 cases in both builds.

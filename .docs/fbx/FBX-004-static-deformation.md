# FBX-004: deterministic static deformation pose

Status: blocked on FBX-003  
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


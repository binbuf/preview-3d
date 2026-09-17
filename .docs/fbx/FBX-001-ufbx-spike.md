# FBX-001: capped-memory and cancellation spike

Status: ready  
Depends on: none  
Unblocks: FBX-002 through FBX-008

## Objective

Satisfy required validation spike 5 in
`design/11-decisions-and-risks.md` for the pinned `ufbx` 0.23.0 build before a
product FBX route is connected. Establish the exact deterministic pose,
allocator, cancellation, instance-sharing, and unsupported-feature decisions
used by the production adapter.

## Context to load

- `design/03-file-formats-and-ingestion.md` — FBX, limits, verification
- `design/09-quality-performance-and-security.md` — Tier-B and cancellation gates
- `design/11-decisions-and-risks.md` — R-02 and required spike 5
- `import-worker/src/ObjAdapter.cpp`
- `shared/model-core/include/model_core/TierALimits.h`
- pinned `ufbx.h` 0.23.0 from the installed vcpkg tree
- `tests/import-isolation/SandboxTestSupport.h`

## Work

1. Add a non-product spike harness and small, redistributable binary/ASCII FBX
   fixtures covering:
   - hierarchy, repeated undeformed mesh instances, pivots, negative and
     non-uniform scale, units, and axis conversion;
   - linear, dual-quaternion, and blended skinning where `ufbx` exposes them;
   - blend shapes and a combined skin-plus-blend case;
   - no animation stack, one stack with a non-zero start, and multiple stacks
     whose authored order is observable;
   - malformed/truncated data, excessive counts/depth, and a deliberately slow
     or large evaluation.
2. Exercise `ufbx_load_memory()` and `ufbx_evaluate_scene()` with explicit temp
   and result allocator limits. Record load, evaluation, and normalized-output
   peak bytes separately; do not infer allocation safety from the Job Object.
3. Verify the intended pose rule: first authored animation stack and its start
   time; otherwise the default/rest scene. Hash finite evaluated positions,
   normals, transforms, and instance relationships across repeated Debug and
   Release runs to prove determinism.
4. Confirm which evaluated attributes are authoritative (`skinned_position`,
   `skinned_normal`, node `geometry_to_world`) and how blend shapes combine with
   skinning. Test inverse-transpose normal behavior and mirrored winding.
5. Measure cancellation during load through `progress_cb`. `ufbx_evaluate_scene`
   has no progress callback in 0.23.0, so explicitly prove that broker
   cancellation terminates/replaces a worker that remains inside evaluation
   after the 500 ms cooperative grace without stalling the viewer or poisoning
   the pool. Record the largest observed non-interruptible interval.
6. Determine a safe sharing key: undeformed geometry may share normalized GPU
   buffers; evaluated geometry may share only when mesh plus effective
   deformer/pose/material-part result is identical. Bound the key/catalog and
   document when duplication is mandatory.
7. Inventory geometry caches, constraints, NURBS, subdivision, layered or
   procedural materials, embedded media, and external references as visible in
   the pinned API. For each, choose accept, warning/fallback, or typed failure in
   accordance with the design; do not implement these policies in the product
   adapter yet.
8. Write measurements and decisions to
   `.docs/fbx/FBX-001-SPIKE-RESULTS.md`, including fixture provenance and exact
   build/hardware details. Update the design/ADR first if the documented limits
   or pose rule cannot be met.

## Constraints

- Do not add `.fbx` to viewer filters, activation, broker production routing,
  installer registration, or release claims.
- The spike must use the pinned library and the real AppContainer/Job/cancel
  path for containment observations; an ordinary in-process benchmark alone is
  insufficient.
- No copyrighted third-party sample may be committed without provenance and a
  redistribution-compatible license.

## Verification

- Spike fixtures pass repeatedly in Debug and Release with identical normalized
  hashes within the chosen numeric tolerance.
- Allocator-limit exhaustion maps to a controlled result, not process-wide OOM.
- Load cancellation is observed at callbacks; evaluation cancellation is
  contained by bounded worker replacement within the existing broker policy.
- A subsequent valid import succeeds after malformed input, allocator failure,
  cancellation, and forced worker termination.
- The results document contains an explicit proceed/revise decision. FBX-002
  may start only on a proceed decision.


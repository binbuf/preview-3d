# FBX-002: normalized scene and instance contract

Status: blocked on FBX-001  
Depends on: FBX-001  
Unblocks: FBX-003

## Objective

Close the gap between the design's normalized `Node`/instance model and protocol
v9's world-baked geometry. Add a format-neutral, copy-then-validated contract
that lets FBX reuse undeformed geometry across instances while preserving
hierarchy/transforms and camera-relative precision.

## Context to load

- `shared/model-core/include/model_core/WireFormat.h`
- `shared/model-core/include/model_core/ControlProtocol.h`
- `shared/import-broker/src/SharedSectionValidator.cpp`
- `shared/import-broker/src/ImportSession.cpp`
- `import-worker/src/BoundedChunkWriter.h`
- `interactive-viewer/src/app/D3D12ImportBridge.{h,cpp}`
- `interactive-viewer/src/app/D3D12ViewerPath.{h,cpp}`
- `interactive-viewer/src/graphics/SceneSnapshot.h`
- `tests/unit/WireFormatTests.cpp`
- `tests/import-isolation/SharedSectionValidatorTextureTests.cpp`
- `tests/hostile-worker/src/AttackModes.cpp`

## Work

1. Define the smallest fixed-width, versioned scene payload needed for bounded
   nodes and mesh instances. It must carry stable IDs, parent relationship,
   visibility, finite double-precision transform data, geometry reference, and
   per-instance material binding. Prefer bounded arrays of fixed-size records;
   never pass importer pointers, strings, or self-describing records.
2. Document dependency-slot semantics for geometry, material, node, and
   instance records. References may cross progressive batches only through the
   bounded generation catalog and must resolve before terminal success.
3. Bump the wire protocol version. Update header/descriptor static assertions,
   producer helpers, validator, generation catalog, protocol fixtures, and
   unknown-version rejection together. There is no v9/v10 mixed-mode fallback.
4. Validate before publication:
   - finite affine matrices and legal visibility/flag masks;
   - unique, nonzero IDs and exact payload lengths;
   - parent/reference existence, acyclic hierarchy, and depth/count limits;
   - topology-correct geometry/material references;
   - transformed finite bounds consistent with referenced local geometry;
   - no dependency graph that can grow without the Tier-B catalog caps.
5. Extend the host bridge and renderer so one uploaded vertex/index resource can
   produce multiple draw records with distinct transforms/material bindings.
   Compute culling, bounds, normal transforms, handedness/cull mode, picking,
   and camera-relative draw matrices correctly for each instance. GPU resource
   retirement remains fence-safe.
6. Add a synthetic scene with one geometry payload and many hierarchical
   instances. Prove one geometry allocation, correct world bounds/draw count,
   stable picking IDs, and no float cast of absolute double translations before
   camera-relative subtraction.
7. Extend hostile-worker attacks for invalid parent IDs, cycles, NaN/Inf
   matrices, illegal topology references, cross-generation IDs, duplicate
   records, oversized tables, late unresolved references, and mutation after
   the host's first read.

## Constraints

- This is a format-neutral contract change, not permission to parse FBX in the
  trusted process.
- Do not regress current GLB/STL/PLY/OBJ output. Existing adapters may keep
  world-baked geometry temporarily, but the new renderer path must coexist and
  be fully tested before FBX uses it.
- Do not retain source hierarchy names or author metadata; the design excludes
  them from normalized/cache data.

## Verification

- Debug and Release Unit and ImportIsolation suites pass.
- Protocol mismatch and every new hostile instance attack are rejected before
  upload/cache consumption.
- A many-instance synthetic scene uploads geometry once and renders/picks every
  instance with correct transformed bounds, including negative scale and a
  large double-precision origin.
- Existing progressive batching, cancellation, coarse/fine handoff, and worker
  pool reuse tests remain green.


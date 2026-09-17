# FBX post-MVP work plan

Status: active execution plan; FBX-006 complete
Prepared: 2026-09-17  
Design authority: [../design/README.md](../design/README.md)

## Decision

Full FBX support is not a safe single-session change. The existing OBJ/MTL
slice supplies useful `ufbx` and texture infrastructure, but the FBX contract
crosses several independently testable boundaries and includes a required
validation spike. Implement it through the tasks in this directory.

## What exists now

- `ufbx` 0.23.0 is pinned through the repository vcpkg overlay and linked only
  into the zero-capability AppContainer import worker.
- The worker pool, brokered primary/sidecar handles, cancellation event,
  progressive shared-section batches, copy-then-validate host path, Tier-B
  limits, PBR material payload, and image decode path already serve OBJ/MTL.
- Protocol v10 has validated node/instance payloads and the renderer's shared
  geometry/instance path. The FBX control opcode, request, source-format ID,
  worker adapter, deterministic static deformation evaluation, and focused
  binary/ASCII fixture corpus are present behind that sandbox boundary.
- FBX-005 is complete: normalized material conversion, embedded images,
  brokered sidecars, sidecar attacks, corrupt/pressure fallback, progressive
  dependencies, layered-texture policy, and distinct per-instance materials
  are qualified in Debug and Release.
- FBX-006 is complete: `.fbx` is enabled by command-line/secondary activation,
  the picker, drag/drop boundary, renderer, Open With discovery, NSIS
  registration, metadata, and packaging documentation. Explorer thumbnails
  remain deliberately absent.
- The Explorer thumbnail project is only a DLL entry-point stub. FBX thumbnail
  work therefore depends on the general Gate 6 provider foundation; it must not
  be smuggled into the viewer slice.

## Product target

"Full FBX" means the original design's bounded **static preview subset**, not
complete FBX authoring fidelity:

- binary and ASCII FBX;
- static hierarchy, reusable instances, polygon geometry, finite transforms,
  source units/up axis, normals, UVs, and vertex colors;
- a deterministic pose at the first authored animation stack's start time, or
  the file's default/rest evaluation when there is no stack;
- supported skin and blend deformation baked into that static pose, with no
  animation playback or retained curves;
- unified base-color/metallic/roughness/emissive/normal material mapping,
  embedded images, and broker-approved local image sidecars;
- warnings or a typed failure for unsupported geometry caches, dynamic
  constraints, NURBS/subdivision geometry, layered/procedural materials, and
  other omitted features according to whether usable visible geometry remains;
- Tier-B resource limits, cancellation, isolation, progressive bounded output,
  recoverable errors, and no viewer-side parser/decoder;
- command line, dialog, drag/drop, secondary activation, renderer, installer,
  Open With/Default Apps registration, documentation, and release evidence.

Animation playback, cameras, lights, geometry-cache evaluation, NURBS or
subdivision tessellation, arbitrary plug-ins/codecs, remote assets, and
unbrokered filesystem access remain out of scope.

## Execution order

| Order | Task | Outcome | Depends on |
| ---: | --- | --- | --- |
| 1 | [FBX-001](FBX-001-ufbx-spike.md) | Prove capped-memory, deterministic deformation, and cancellation behavior | Current main branch |
| 2 | [FBX-002](FBX-002-scene-instance-contract.md) | Add a validated scene/instance contract and renderer sharing | FBX-001 decisions |
| 3 | [FBX-003](FBX-003-worker-adapter.md) | Import undeformed static FBX through the sandbox without exposing `.fbx` to users | FBX-002 |
| 4 | [FBX-004](FBX-004-static-deformation.md) | Evaluate and bake the deterministic skin/blend pose | FBX-003 |
| 5 | [FBX-005](FBX-005-materials-and-textures.md) | Complete unified PBR, embedded images, and brokered image sidecars | FBX-004 |
| 6 | [FBX-006](FBX-006-product-integration.md) | Enable every viewer/open/installer surface | FBX-005 |
| 7 | [FBX-007](FBX-007-qualification.md) | Add corpus, fuzz, security, performance, packaging, and release evidence | FBX-006 |
| 8 | [FBX-008](FBX-008-thumbnail-provider.md) | Add the original-MVP Explorer thumbnail path | General Gate 6 provider foundation and FBX-007 |

Tasks are deliberately sequential. FBX-002 changes the normalized scene
contract consumed by later tasks; FBX-003 through FBX-005 build one adapter in
layers; FBX-006 must not advertise the extension before the parser and feature
policy are complete. FBX-006 completed the product integration and unblocks
FBX-007 qualification.
Within a task, implementation and its focused tests land together.

## Completion boundaries

- **Gate 4 FBX viewer slice complete:** FBX-001 through FBX-007 are complete.
  `.fbx` opens through every viewer activation surface, the installed product
  registers it without taking defaults, and the release evidence satisfies the
  Tier-B format requirements.
- **Original-MVP FBX family complete:** FBX-008 is also complete after the
  general thumbnail-provider foundation exists. Until then documentation must
  explicitly say that FBX Explorer thumbnails are unavailable.

## Cross-task rules

1. Do not parse or decode FBX in `Preview3D.exe`. All `ufbx` and image work stays
   in `Preview3DImportWorker.exe`; thumbnail parsing later stays in the isolated
   Shell COM DLL under its separate limits.
2. The worker never receives a path or opens a dependency. External images use
   `RequestSidecarFile`; geometry caches are not requested.
3. Keep the trusted host's copy-then-validate rule. No downstream component may
   retain or reread shared-section memory.
4. Check counts and multiplication before allocation. `ufbx` allocator limits
   and the Job Object are backstops, not substitutes for product Tier-B limits.
5. Do not enable `.fbx` in user-visible entry points before FBX-005 is complete
   and its recovery/security tests pass.
6. Any protocol layout change bumps the version, rejects older versions, updates
   static assertions/fixtures/fuzz seeds, and reruns the hostile-worker suite.
7. Existing source-format behavior and user changes must remain intact. Avoid a
   broad `ufbx` rewrite of the working OBJ path unless a tested shared helper is
   required.
8. A failed spike changes the design/ADR and this plan before implementation;
   it does not justify relaxing isolation, memory, or cancellation invariants.

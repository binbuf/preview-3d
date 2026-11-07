---
timeoutMin: 360
verify: x64\Release\Tests.ImportIsolation.exe "[reader-fallback]"
---
# T08 — Compatibility readers and bounded fallback

## Goal
When a native parser rejects a valid file that another shipped parser can read,
the user gets a real compatibility path instead of a terminal failure. Add
lower-scored Assimp compatibility readers for glTF, OBJ, FBX, and 3MF, and a
closed retry policy that never retries a security, containment, or resource
failure.

## Context (read first)
- `import-worker/src/AssimpAdapter.cpp` and `AssimpImportWorker.cpp` — the
  bounded Assimp adapter, admission, broker `IOSystem`, and current seven
  format hints.
- `import-worker/src/WorkerRequestDispatch.cpp` — the closed worker request
  opcode and `ParseAssimpFileRequest` route.
- `shared/model-core/include/model_core/ControlProtocol.h` — `AssimpFormat`
  and the closed request discriminator.
- `interactive-viewer/src/app/D3D12ImportBridge.cpp` — `RunImport`,
  `SourceFormat`, `AssimpFormatForSource`, and the host/worker routing.
- `shared/import-broker/include/import_broker/ImportSession.h` — the session
  result and the existing USD atomic fallback pattern.
- `shared/model-core/include/model_core/ImportError.h` — the closed error
  taxonomy the retry policy must classify.
- F3D reference: `plugins/assimp/CMakeLists.txt:42-118` declares FBX, COLLADA,
  DXF, OFF, DirectX, 3MF, and AMF readers; `library/src/factory.cxx.in`
  selects by score. F3D does not silently retry after a parse failure; it
  selects by score and offers `--force-reader`. Mirror that model, not a blind
  retry loop.

## Scope
- [ ] Extend the closed `AssimpFormat` discriminator with glTF, OBJ, FBX, and
  3MF compatibility hints. The hint is product-owned; never pass a file path
  or user text to Assimp.
- [ ] Extend Assimp admission for the new containers:
  - glTF/GLB: bounded JSON/GLB header and buffer-view preflight before Assimp;
  - OBJ: bounded text admission and broker-only MTL/texture sidecars;
  - FBX: binary/ASCII magic and bounded header preflight;
  - 3MF: bounded OPC/ZIP preflight (reuse the existing 3MF preflight or a
    shared bounded ZIP reader; do not trust Assimp’s ZIP handling).
- [ ] Add reader descriptors (T07) for the compatibility readers with scores
  below the native readers:
  - `gltf-assimp`, `obj-assimp`, `fbx-assimp`, `3mf-assimp`.
  Each declares the same extensions as its native family and
  `isCompatibility=true`.
- [ ] Implement the closed fallback policy in the trusted broker/app layer:
  - eligible only for `UnsupportedFormat`, `UnsupportedRequiredFeature`, and
    `EmptyGeometry` when the selected reader is the native one;
  - never for `UnsafeReference`, `FileUnavailable`, `FileChanged`,
    `ResourceLimit`, `ArchiveLimit`, `PrimarySourceLimit`,
    `AggregateSourceLimit`, `ScratchLimit`, `Cancelled`, `WorkerCrashed`,
    `WorkerTimedOut`, or `ImportProtocolViolation`;
  - at most one fallback per generation;
  - the fallback runs in the same sandbox with the same budgets and sidecar
    policy, and publishes into the same generation only before any batch is
    accepted.
- [ ] Prefer an explicit user action over a silent retry: the failure card
  offers “Try compatibility reader” when an eligible alternate exists. A
  silent retry is acceptable only for the closed eligible set and must be
  recorded in diagnostics with both reader names.
- [ ] Add `[reader-fallback]` cases:
  - a native-rejected glTF/OBJ/FBX/3MF fixture opens through the compatibility
    reader;
  - a security/containment failure does not retry;
  - a resource-limit failure does not retry;
  - a malformed file does not retry (or, if the policy includes
    `MalformedData`, prove the alternate is bounded and the failure is not
    hidden);
  - only one fallback occurs;
  - a forced compatibility reader works without the native attempt;
  - the diagnostic names both readers and the final reader.
- [ ] Add fixtures and fuzz seeds for each new Assimp container. Reuse the
  existing `AssimpFuzz` dispatcher or add per-container seeds.
- [ ] Update `docs/FORMAT-SUPPORT.md`, `README.md`, and the task README.

## Out of scope
- Replacing the native glTF/OBJ/FBX/3MF adapters. They remain the default.
- Adding new primary formats or new thumbnail families.
- Automatic fallback for USD (already has the fast-path/compatibility-host
  pattern) or STEP.
- Unbounded retry chains across multiple readers.

## Design notes
- Assimp is a large historical CVE surface. The new hints must keep the
  existing broker `IOSystem`, no default `IOSystem`, no plug-ins, no file
  writes, no animation, and the same AppContainer/Job Object containment.
- Do not let a compatibility reader bypass a native preflight that exists for
  security. If the native preflight rejects a file as unsafe, the fallback must
  not try to reinterpret it.
- The fallback must not mix producers: if the native reader published any
  candidate batch, the generation is already committed to it. Retry only
  before publication, matching the USD `UnsupportedComposition` rule.
- A forced compatibility reader is an explicit user choice, not an automatic
  retry; it still runs every admission and budget check.
- Keep the reader table and the worker discriminator in sync with a unit test
  so a new hint cannot be added to one side only.
- The compatibility reader runs only inside the existing worker sandbox. It
  must not add a path open, a default Assimp `IOSystem`, a plug-in load, a
  network call, a child process, or a file write.
- The retry decision belongs to the trusted broker/app layer and may only
  relaunch the same bounded worker/host with the same AppContainer, Job Object,
  budgets, and sidecar policy. It must never switch to a more permissive mode
  or a different security boundary.
- If a native preflight rejects a file as unsafe, the compatibility reader is
  not attempted. The retry policy is for representational gaps, not for
  containment failures.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe "[reader-fallback]"` passes.
- [ ] `x64\Debug\Tests.ImportIsolation.exe "[reader-fallback]"` passes.
- [ ] Each compatibility reader opens a native-rejected fixture.
- [ ] Security/resource failures never retry.
- [ ] Only one fallback can occur per generation.
- [ ] The failure/diagnostic path names both readers.
- [ ] Fuzz seeds run clean on the new containers.
- [ ] No path/network/file-write/child-process authority was added.

## Hand-off
_(filled in by the implementing session: new hints, admission rules, retry policy, fixtures, fuzz targets, and any native-reader rejection that remains terminal)_

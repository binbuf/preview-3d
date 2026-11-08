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
- [x] Extend the closed `AssimpFormat` discriminator with glTF, OBJ, FBX, and
  3MF compatibility hints. The hint is product-owned; never pass a file path
  or user text to Assimp.
- [x] Extend Assimp admission for the new containers:
  - glTF/GLB: bounded JSON/GLB header and buffer-view preflight before Assimp;
  - OBJ: bounded text admission and broker-only MTL/texture sidecars;
  - FBX: binary/ASCII magic and bounded header preflight;
  - 3MF: bounded OPC/ZIP preflight (reuse the existing 3MF preflight or a
    shared bounded ZIP reader; do not trust Assimp’s ZIP handling).
- [x] Add reader descriptors (T07) for the compatibility readers with scores
  below the native readers:
  - `gltf-assimp`, `obj-assimp`, `fbx-assimp`, `3mf-assimp`.
  Each declares the same extensions as its native family and
  `isCompatibility=true`.
- [x] Implement the closed fallback policy in the trusted broker/app layer:
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
- [x] Prefer an explicit user action over a silent retry: the failure card
  offers “Try compatibility reader” when an eligible alternate exists. A
  silent retry is acceptable only for the closed eligible set and must be
  recorded in diagnostics with both reader names.
- [x] Add `[reader-fallback]` cases:
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
- [x] Add fixtures and fuzz seeds for each new Assimp container. Reuse the
  existing `AssimpFuzz` dispatcher or add per-container seeds.
- [x] Update `docs/FORMAT-SUPPORT.md`, `README.md`, and the task README.

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
- [x] `x64\Release\Tests.ImportIsolation.exe "[reader-fallback]"` passes.
- [x] `x64\Debug\Tests.ImportIsolation.exe "[reader-fallback]"` passes.
- [x] Each compatibility reader opens a native-rejected fixture.
- [x] Security/resource failures never retry.
- [x] Only one fallback can occur per generation.
- [x] The failure/diagnostic path names both readers.
- [x] Fuzz seeds run clean on the new containers.
- [x] No path/network/file-write/child-process authority was added.

## Hand-off

### What landed

- **Closed hints.** `model_core::AssimpFormat` gained `Gltf`, `Obj`, `Fbx`,
  `ThreeMf` and `kAssimpFormatCount`; `IsKnownAssimpFormat` and the worker
  request validator accept them. `AssimpFuzz` dispatches with
  `kAssimpFormatCount` instead of a hard-coded modulus.
- **Reader table.** `ReaderCatalog.h` adds `gltf-assimp`, `obj-assimp`,
  `fbx-assimp`, `3mf-assimp` (score 35, `isCompatibility=true`, same
  extensions as the native family). `CompatibilityReaderForFamily` and
  `AssimpFormatForCompatibilityReader` are the table/discriminator sync
  points. `--list-readers` and Open As now show 15 readers; the four families
  have a default and one alternate.
- **Admission.** `AssimpAdapter.cpp` adds:
  - a bounded JSON/GLB scanner for glTF (header, chunk table, buffer views,
    external `.bin`/data-URI checks, 256 MiB JSON cap);
  - bounded OBJ text admission (512 MiB, `v` + `f`/`l`/`p` evidence);
  - FBX binary magic/version and ASCII `FBXVersion` admission (512 MiB);
  - 3MF `InspectThreeMfOpc` reuse (structural OPC/ZIP only, no semantic
    required-extension rejection).
  The broker `IOSystem` allows only images plus glTF `.bin` and OBJ `.mtl`;
  everything else stays `UnsafeReference`.
- **Fallback policy.** `import_broker/ReaderFallback.h` owns the closed route,
  eligible-error set and stable names. `ImportSessionRequest::readerFallback`
  is a closed route; `RunImportSession` dispatches to
  `RunImportSessionWithFallback`. The native attempt runs first; exactly one
  compatibility attempt runs only for `UnsupportedFormat`,
  `UnsupportedRequiredFeature` or `EmptyGeometry`, only with `batchCount == 0`
  and no delivered batch. The compatibility copy keeps the same worker,
  AppContainer, Job Object, budgets, source handle, sidecar policy and
  cancellation event; it clears coarse/detail replay and format-specific
  options. Result fields record native/compatibility/final reader names,
  attempt count and a path-free diagnostic.
- **App/UI.** `D3D12ImportBridge` adds `SourceFormat::AssimpGltf/Obj/Fbx/
  ThreeMf`, `SourceFormatForReader`, `ReaderFallbackForSource`,
  `CompatibilityReaderForFailedSource`, reader provenance on `ImportResult`,
  and an `allowAutomaticReaderFallback` parameter (false for a forced reader).
  `Preview3D` relabels the failure-card Retry button to **Try compatibility
  reader** when an eligible alternate is offered, routes that action to the
  forced compatibility reader, and reports the final reader in the info panel
  and diagnostics. A successful automatic fallback shows an "Opened with the
  … compatibility reader" notice.
- **Fixtures.** `tests/fixtures/assimp-spike/generate_fixtures.py` now writes
  `box.gltf`, `required-extension.gltf`, `unsafe-buffer.gltf`,
  `required-extension-missing-buffer.gltf`, `resource-dag.gltf`,
  `malformed.gltf`, `box.obj`, `missing-vt.obj`, `line-only.obj`, `box.fbx`,
  `line-only.fbx`, `malformed.fbx`, `box.3mf`, `required-extension.3mf` and
  `malformed.3mf`.
- **Tests.** `tests/import-isolation/ReaderFallbackTests.cpp` (tag
  `[reader-fallback]`, 9 cases / 237 assertions) covers the closed policy,
  glTF/3MF/OBJ/FBX native-rejected → compatibility opens, security and
  resource/malformed no-retry, exactly-one-attempt, forced compatibility,
  app fallback and the explicit-offer path. `ReaderSelectionTests.cpp` now
  expects 15 readers and the four alternates.
- **Fuzz.** `prepare_assimp_seeds.py` emits 25 seeds including the four new
  containers; `AssimpFuzz.vcxproj` links `ThreeMfOpcPreflight.cpp`.
- **Docs.** `docs/FORMAT-SUPPORT.md`, `README.md`,
  `docs/tasks/0.8.0/README.md`, `docs/design/03-file-formats-and-ingestion.md`
  and ADR-0102.

### Native rejections that remain terminal

- `UnsafeReference`, `FileUnavailable`, `FileChanged`, `ResourceLimit`,
  `ArchiveLimit`, `PrimarySourceLimit`, `AggregateSourceLimit`,
  `ScratchLimit`, `Cancelled`, `WorkerCrashed`, `WorkerTimedOut` and
  `ImportProtocolViolation` never retry.
- `MalformedData` never retries; the compatibility attempt is only for the
  three representational codes above.
- A forced reader never retries automatically. The failure card offers the
  explicit action only when the error is in the eligible set and the family
  has a shipped alternate.
- The compatibility attempt itself is final; there is no second fallback.
- FBX NURBS/subdivision/geometry-cache-only content remains terminal: the
  native adapter reports `UnsupportedRequiredFeature` and Assimp does not
  evaluate those constructs either, so the one compatibility attempt also
  fails typed. The same is true for any native representational gap that
  Assimp cannot represent.

### Checks (2026-11-07)

- Release `x64\Release\Tests.ImportIsolation.exe "[reader-fallback]"` —
  237 assertions, 9 cases, passed.
- Debug `x64\Debug\Tests.ImportIsolation.exe "[reader-fallback]"` —
  237 assertions, 9 cases, passed.
- Release `[assimp]` — 164 assertions, 14 cases, passed.
- Release `[assimp-primitives]` — 79 assertions, 7 cases, passed.
- Release `[valid-file]` — 163 assertions, 2 cases, passed.
- Release `[compat]` — 600 assertions, 2 cases, passed.
- Release `[sidecar]` — 277 assertions, 13 cases, passed.
- Release `[sidecar-resolver]` — 329 assertions, 33 cases, passed.
- Release and Debug `Tests.Unit.exe "[reader-selection]"` — 823 assertions,
  8 cases, passed.
- Full Release `Tests.Unit.exe` — 166,411 assertions, 535 cases, passed.
- `Preview3D.exe --list-readers` — 15 lines in stable name order.
- `AssimpFuzz.exe -runs=1` over the 25 prepared seeds — clean (27 runs, no
  crash).
- `Preview3D.vcxproj` Release and Debug build.

### Build notes

- Rebuild `Preview3DImportWorker.vcxproj` before the ImportIsolation suite; the
  test launches that binary.
- Delete `tests/import-isolation/x64/<config>` after `D3D12ImportBridge.h`
  changes (the vcxproj does not track that header for every object).
- The worker's stderr is null under the sandbox, so worker-side diagnostics are
  not visible in the test console; use the fuzz target or the typed result.

---
verify: x64\Release\Tests.Unit.exe
---
# T04 — Freeze provider interfaces and the shared model-core subset

## Goal
Define the interfaces every adapter, the sampler and the rasterizer implement, and freeze the exact
shared source subset the DLL compiles — so foundation tasks and family adapters build against one
stable contract.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — COM classes, geometry sampling and CPU renderer contracts.
- `docs/design/03-file-formats-and-ingestion.md` — the normalized scene/limits the provider must honour.
- `docs/design/adr/0004-share-source-not-state.md` — source sharing, no shared process/state.
- `shared/model-core/include/model_core/` — `WireFormat.h`, `MaterialPayload.h`, and the checked helpers.
- `import-worker/src/GltfAdapter.h`, `import-worker/src/StlAdapter.*`, `import-worker/src/PlyAdapter.*` — implemented viewer adapters used as policy references.
- `thumbnail-provider/Preview3DThumbnailProvider.vcxproj` — the current stub project.

## Scope
- [ ] Define a family-adapter interface: bounded `Initialize(stream/limits/deadline)`, parse-to-bounded-intermediate, geometry enumeration (triangles and points with positions/normals/colors/transform), material enumeration, and typed failure — emitting only product-owned types.
- [ ] Define the geometry-sample and material-fallback types shared by adapters, sampler and rasterizer.
- [ ] Freeze the CLSID→family routing table as a single source (shared with registration in T41) with no runtime sniffing.
- [ ] Enumerate the exact `shared/model-core` and product-parser source files compiled into the DLL, classify each as move/duplicate/exclude, and hand the list to T07, which performs the extraction so they compile without viewer/worker-only dependencies.
- [ ] Record the interface in `docs/design/05-thumbnail-provider.md` (or a new `design/interfaces.md`) with names and signatures.

## Out of scope
- Implementing the interfaces (→ T11–T17, T21–T34).
- Changing normalized wire formats used by the viewer/worker (not needed; the provider has no IPC).

## Design notes
- Interfaces must be implementable with per-call arenas and no process-global mutable caches.
- No third-party type may appear in an interface signature.
- Keep the adapter surface small enough that each family task is an isolated implementation.

## Done when
- [ ] Header(s) for the adapter/sampler/raster contracts exist and compile in the provider project.
- [ ] A build of `Preview3D.slnx` Debug x64 still succeeds with the new headers included.
- [ ] The shared-source list and routing table are documented and referenced by later tasks.
- [ ] Docs touched: `design/05-thumbnail-provider.md` or new `design/interfaces.md`.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
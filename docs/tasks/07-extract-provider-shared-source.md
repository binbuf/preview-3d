---
verify: x64\Release\Tests.ImportIsolation.exe [stl-import],[ply-import],[gltf-import]
---
# T07 - Extract the provider-shared parser source subset

## Goal
Turn ADR-0004's "compile the same source files into the DLL" from an intention into a
compilable boundary: the model-core helpers and product parser cores the provider needs
are consumable by `Preview3DThumbnailProvider.vcxproj` without dragging in any
import-worker, broker, host or viewer dependency.

## Context (read first)
- `docs/design/adr/0004-share-source-not-state.md` - the source-sharing decision this task implements.
- `docs/tasks/04-freeze-provider-interfaces.md` - froze the exact shared-source list and routing table.
- `shared/model-core/include/model_core/` - `WireFormat.h`, `MaterialPayload.h`, `VertexLayouts.h`, `TierALimits.h` and the checked helpers.
- `import-worker/src/StlAdapter.*`, `PlyAdapter.*`, `AsciiTokenizer.*` - product parsers to reuse.
- `import-worker/src/ChunkBatchSink.*`, `SidecarFileClient.*`, `WorkerRequestDispatch.*`, `GenerationWorker.*`, `CoarseSampler.h` - worker-only dependencies that must not be pulled into the DLL.
- `thumbnail-provider/Preview3DThumbnailProvider.vcxproj` - the project the subset must compile into.

## Scope
- [ ] Take the T04 source list and classify every file as (a) move to a shared, format-agnostic location, (b) duplicate a behaviour-identical provider copy, or (c) excluded because it is intrinsically worker-coupled.
- [ ] Decouple or stub the worker-only dependencies (`ChunkBatchSink`, `SidecarFileClient`, `ControlProtocol`/`ControlChannelIo`, `GenerationWorker`, `CoarseSampler`) so the extracted parser cores compile with no broker/worker/viewer header.
- [ ] Establish the extracted subset as a single source set (a `shared/parser-core/` directory or an explicit include list) and add it to `Preview3DThumbnailProvider.vcxproj`.
- [ ] Prove the DLL still builds and the import worker is unchanged: `Tests.ImportIsolation.exe` still passes after the move.
- [ ] Record, in `design/05-thumbnail-provider.md` or `design/interfaces.md`, exactly which files were moved, which were duplicated, and which worker-only dependencies were cut.

## Out of scope
- Writing any provider adapter, routing, sampler or rasterizer (-> T11-T17, T21-T34).
- Changing normalized wire formats or import-worker behaviour (the provider has no IPC).
- Introducing a standalone static library project; ADR-0004 keeps source-per-consumer until that proves fragile.

## Design notes
- Extraction must be behaviour-preserving: a change to a shared parser is a change to both the worker and the provider.
- No third-party type or worker-only type may appear in a header the provider compiles.
- Prefer moving genuinely format-agnostic code once; duplicate only the small policy shims where worker coupling is intrinsic.
- The extract is a compile boundary, not a de-duplication project: correctness of the existing worker suite outranks elegance.

## Done when
- [ ] `msbuild Preview3D.slnx /p:Configuration=Debug /p:Platform=x64` and the Release equivalent succeed with the extracted subset compiled into the provider project.
- [ ] `x64\Release\Tests.ImportIsolation.exe "[stl-import],[ply-import],[gltf-import]"` (the `verify:` command) passes, proving the extracted parser cores in the worker were not disturbed. Do not gate on the unfiltered suite, which has pre-existing unrelated USD/large-scan failures; record in Hand-off that those are unchanged from the pre-task baseline.
- [ ] The moved/duplicated/cut file list is recorded and referenced by T21-T34.
- [ ] Docs touched: `design/05-thumbnail-provider.md` or `design/interfaces.md`; ADR-0004 consequence.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
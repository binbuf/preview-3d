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

**Landed.** A new `shared/parser-core/` source set is compiled into **both** `Preview3DImportWorker.exe`
and `Preview3DThumbnailProvider.dll`: `include/parser_core/AsciiTokenizer.h` + `src/AsciiTokenizer.cpp`
(moved out of `import-worker/src`, namespace renamed `import_worker`→`parser_core`),
`StlParserCore.h/.cpp`, and `PlyParserCore.h/.cpp`. The STL core holds the binary-STL layout constants,
`ReadFloatLE`/`ReadU32LE`, `Vec3`/`Cross`/`Dot`/`IsFinite`, `NormalizeStlFacet` (the finite/degenerate
drop and supplied-vs-flat normal policy), `DecodeStlBinaryHeader`, and `IsAsciiStl`. The PLY core holds
the `PlyScalarType` model, `ScalarByteSize`/`ParseScalarTypeName`/`NormalizeColor`/`ReadScalarAsDouble`,
the `PlyProperty`/`PlyElement`/`PlyFormat`/`PlyHeader` model, `ParseHeader`, `ReadBytes`, `IsAsciiPly`,
and the float `Vec3f`/`Cross`/`Dot`. None of the three files includes an IPC, mapping, broker, worker
or viewer header: their only dependencies are std, `model_core/ImportError.h`,
`model_core/TierALimits.h` and `platform/CheckedMath.h`.

The worker consumes the moved code (the old `import-worker/src/AsciiTokenizer.*` are deleted):
`StlAdapter.cpp`/`PlyAdapter.cpp` include `parser_core/...`, `StlAdapter.cpp::ProcessFacet` delegates to
`NormalizeStlFacet` (both scalar and parallel binary paths), and the ascii detection forwards to
`parser_core::IsAsciiStl`. The provider `.vcxproj` adds `..\shared\parser-core\include` and compiles the
three `.cpp` with `<PrecompiledHeader>NotUsing</PrecompiledHeader>` (the worker has no PCH); the worker
`.vcxproj` swaps the old file entries for the shared ones. `Tests.Unit.vcxproj` compiles the same three
files plus `tests/unit/ParserCoreTests.cpp` (tag `[parser]`, 6 cases).

**Classification** (recorded in `docs/design/interfaces.md` "Shared source subset"): **move** →
`AsciiTokenizer.*`, `StlParserCore.*`, `PlyParserCore.*` (now under `shared/parser-core/`).
**duplicate** → `StlAdapter.*`/`PlyAdapter.*` (reclassified from T04's move) and the remaining
family decoder cores; their wire/Tier A/coarse/mapped coupling is intrinsic worker policy and cannot
cross the DLL boundary. **exclude** → `BoundedChunkWriter.h`, `ChunkBatchSink.*`, `CoarseSampler.h`,
`SidecarFileClient.*`, `GenerationWorker.*`, `WorkerRequestDispatch.*`, `main.cpp`,
`ControlProtocol.h`/`ControlChannelIo.*`, `WireFormat.h`, `Checksum.h`, `VertexLayouts.h`,
`GeometryBounds.h`, `MappedFile.*`, `FileIdentity.h`, `ModelCore.h/.cpp`. The Tier A count constants
T04 asked to extract already live in the shared `model_core/TierALimits.h`
(`kTierATriangleLimit`/`kTierAPointLimit`/`kTierAVertexLimit`).

**Deviations.** `StlAdapter.*`/`PlyAdapter.*` were classified duplicate, not moved, because a literal
move would drag `WireFormat.h`/`Checksum.h`/`VertexLayouts.h`/`ControlProtocol.h`/`CoarseSampler.h`/
`windows.h` into the DLL — every one T04 excludes (ADR-0012). The provider's T21/T23 adapters will
reuse the shared parser-core and emit `IGeometrySink`/`IMaterialSink` samples instead of wire chunks,
so the extraction still gives the DLL the product parser cores it needs while keeping the worker's
proven IPC/streaming behaviour byte-for-byte unchanged. `ParserCoreTests.cpp` was added as the task's
regression coverage. No wire format, import-worker behaviour or third-party boundary changed.

**Checks (actual commands / results).**
- `msbuild thumbnail-provider\Preview3DThumbnailProvider.vcxproj /p:Configuration=Debug|x64` and
  `Release`: clean, shared parser-core `.cpp` compiled and linked into
  `Preview3DThumbnailProvider.dll` (`/W4 /WX`).
- `msbuild import-worker\Preview3DImportWorker.vcxproj /p:Configuration=Debug|x64` and `Release`:
  clean.
- `msbuild Preview3D.slnx /p:Configuration=Debug /p:Platform=x64` and the Release equivalent: all
  projects build except the **pre-existing, unrelated** `compatibility-host-step`
  `C1083 'BRepBndLib.hxx'` (OCCT `x64-windows-static-md` triplet not installed here; unchanged from
  T04's baseline).
- `x64\Release\Tests.ImportIsolation.exe "[stl-import],[ply-import],[gltf-import]"` (the `verify:`
  command): **All tests passed — 69 cases / 1564 assertions** (exit 0), proving the worker's parser
  cores were not disturbed.
- `x64\Release\Tests.Unit.exe`: **140 cases / 76245 assertions green** (includes the 6 new `[parser]`
  cases and the 11 `[provider]` scaffold cases). Debug `[parser]`: 6 cases green.
  (Build `Tests.Unit` with `/p:SolutionDir=<root>\` or via the solution so `PREVIEW3D_PROVIDER_DLL`
  expands.)

**Next must know.** T21/T23 implement `IFamilyAdapter` on top of `parser_core::StlParserCore`/
`PlyParserCore` (they must not include a worker/broker header; add any provider-only sample emission in
`thumbnail-provider/`). `ParserCoreTests.cpp` is the regression set to extend if a shared primitive
changes. ADR-0012 records the boundary; interfaces.md and design/05 name the moved/duplicated/cut
files.
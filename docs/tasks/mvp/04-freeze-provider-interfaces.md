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

**Landed.** Five frozen headers in `thumbnail-provider/`: `ProviderTypes.h` (product value types,
`Family`, `BoundedSource`, `NeutralMaterial()`), `FamilyAdapter.h` (`IFamilyAdapter`,
`IGeometrySink`, `IMaterialSink`, `AdapterInput`), `GeometrySampler.h` (`IGeometrySampler`,
`SampledGeometry`), `CpuRasterizer.h` (`ICpuRasterizer`, `RasterRequest`, `RasterImage`), and
`FamilyRouting.h` (the eight CLSIDs, extension lists, and the `{E357FCCD-...}` thumbnail ShellEx
GUID). `ProviderContracts.cpp` is an interface-only TU that compiles them and static_asserts the
roster. The provider project now includes `..\shared\model-core\include` and all six files.
Signature-level doc: `docs/design/interfaces.md`; decision: `docs/design/adr/0009-...md`;
`design/05-thumbnail-provider.md` cross-links both.

**Deviations.** `ProviderLimits`, `Deadline` and `AllocationLedger` are forward-declared here and
left for T06 to define under those exact names, so T06 owns the concrete budget/deadline/ledger
types instead of T04 duplicating them; the adapter/sampler/raster signatures reference them by
pointer. The adapter emits samples through `IGeometrySink`/`IMaterialSink` callbacks (cap stop =
`false`, not an error) rather than returning a container, keeping the adapter surface small and
streaming. `RasterImage` carries a premultiplied BGRA buffer rather than an `HBITMAP` so the
rasterizer interface is GDI/Windows-free and unit-testable; T15/T13 create the DIB/HBITMAP and set
`WTS_ALPHATYPE`.

**Checks.**
- `msbuild thumbnail-provider\Preview3DThumbnailProvider.vcxproj /p:Configuration=Debug|x64` and
  Release: clean (0 warnings, /W4 /WX).
- `msbuild Preview3D.slnx /p:Configuration=Release /p:Platform=x64`: provider + `Tests.Unit.exe`
  build; **pre-existing, unrelated failure** in `compatibility-host-step` (`C1083 BRepBndLib.hxx`):
  its vcpkg `x64-windows-static-md` OCCT triplet is not installed in this environment (only
  `x64-windows` exists) and the vcxproj hardcodes static-md. Same failure occurs without T04.
- `x64\Release\Tests.Unit.exe`: 123 cases / 74004 assertions, all passed.

**Next must know.** T06 defines `ProviderLimits`/`Deadline`/`AllocationLedger` under the frozen
names. T07's exact extraction list and move/duplicate/exclude classification is in
`docs/design/interfaces.md` ("Shared source subset"); it must decouple `StlAdapter.*`/`PlyAdapter.*`
from `BoundedChunkWriter.h`/`ChunkBatchSink.h` and extract the `kTierA*` constants. T41 must consume
`FamilyRouting.h` and assert the installed values match it. The `compatibility-host-step` OCCT
static-md install gap blocks only the full-solution build, not the verify command; it is recorded in
`docs/PROGRESS.md` Follow-ups.
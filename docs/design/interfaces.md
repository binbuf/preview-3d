# Provider contracts and shared source

T04 freezes the contracts every family adapter, the geometry sampler and the CPU
rasterizer implement, and the exact shared source subset
`Preview3DThumbnailProvider.dll` compiles. Later foundation tasks (T11–T17) and
family tasks (T21–T34) build against this document and the headers it names;
[ADR-0009](adr/0009-provider-interface-and-shared-source-boundary.md) records
the decision, and [05-thumbnail-provider.md](05-thumbnail-provider.md) remains
the behavioural authority.

The headers live in `thumbnail-provider/` and are compiled into the provider
project by `thumbnail-provider/ProviderContracts.cpp` (an interface-only
translation unit, no runtime behavior):

| Header | Contents |
| --- | --- |
| `ProviderTypes.h` | Value types, `Family`, forward-declared T06 services, `BoundedSource`, `NeutralMaterial()`. |
| `FamilyAdapter.h` | `IFamilyAdapter`, `IGeometrySink`, `IMaterialSink`, `AdapterInput`. |
| `GeometrySampler.h` | `SampledGeometry`, `IGeometrySampler`. |
| `CpuRasterizer.h` | `RasterRequest`, `RasterImage`, `ICpuRasterizer`. |
| `FamilyRouting.h` | The frozen CLSID→family table and the thumbnail-handler ShellEx GUID. |
| `ProviderLimits.h` | T06 frozen budget constants and the checked file-derived integer/range helpers. |
| `Deadline.h` | T06 monotonic 750 ms p95 / cooperative 2 s `Deadline` with `expired()`/`remaining()`/`Checkpoint()`. |
| `AllocationLedger.h` | T06 process-wide atomic product-owned allocation ledger plus the RAII `AllocationReservation`. |
| `ProviderErrors.h` | T06 HRESULT mapping (`ProviderOutcome`, `HresultFor`, `ClassifyError`, `HresultForError`). |

## Product-owned types only

No third-party parser type (fastgltf, ufbx, lib3mf, TinyUSDZ, OCCT, Draco,
KTX/Basis, WebP) appears in any signature. Output uses `preview3d::provider`
types and the shared `model_core` types already owned by the product:

- `ErrorCode` = `model_core::ImportErrorCode` ([03](03-file-formats-and-ingestion.md#error-taxonomy)).
- `model_core::MaterialPayload` for a material; `NeutralMaterial()` supplies the
  neutral fallback and the rasterizer's substitute for an unset material.
- `VertexSample` (local float position, optional unit normal, linear RGBA color),
  `TriangleSample` and `PointSample` (with a double per-sample origin and a
  1-based `materialIndex`), and `Bounds` (double, `valid` flag).

## Adapter contract

`preview3d::provider::IFamilyAdapter` is a small, one-call lifecycle so each
family task is an isolated implementation:

```cpp
ErrorCode Initialize(const AdapterInput& input) noexcept; // bounded setup
ErrorCode Parse() noexcept;                               // bounded intermediate
ErrorCode EnumerateMaterials(IMaterialSink& sink) noexcept;
ErrorCode EnumerateGeometry(IGeometrySink& sink) noexcept;
void      Reset() noexcept;                               // noexcept teardown
```

- `AdapterInput` carries a `BoundedSource*`, the T06 `ProviderLimits*`,
  `Deadline*` and `AllocationLedger*`, plus the routed `Family`. The adapter
  owns none of them.
- `IGeometrySink::OnTriangle/OnPoint` return `false` to ask the adapter to stop
  promptly at a cap; a cap stop is not a failure (the adapter returns `None`).
- `IMaterialSink::OnMaterial` emits a 1-based index that must match the indices
  written into samples; `0` is reserved for the neutral fallback.
- `Initialize` takes exactly one bounded source and rejects a second
  initialization; adapters never open a path or resolve a sidecar. Any non-`None`
  result must leave no partially trusted state.
- `Reset` releases every per-call resource; destructors are `noexcept`.

`BoundedSource` is implemented once by T12 over `IInitializeWithStream`: a
validated `Size()`, `Seekable()`, checked `ReadAt(offset, dest)` (false on a
short read, an out-of-range read, an exceeded limit or an expired deadline), and
an optional `ContiguousView()` for adapters that need one buffer within the
128 MiB backing cap.

## Sampler contract

`IGeometrySampler` is fed by an adapter's `EnumerateGeometry` and produces the
deterministic representative set:

```cpp
void Begin(std::uint64_t sourceSeed) noexcept;
bool AddTriangle(const TriangleSample&) noexcept;
bool AddPoint(const PointSample&) noexcept;
const SampledGeometry& Result() const noexcept; // triangles, points, bounds
```

The seed is stable per source so Explorer's cache stays consistent. T14 owns the
spatial/reservoir policy, the 2 M-triangle / 6 M-point inspect caps, the 250 k
rasterized-sample cap, and charging retained storage against the T06 ledger.

## Rasterizer contract

`ICpuRasterizer::Render(const RasterRequest&, RasterImage&) noexcept` consumes
`SampledGeometry`, a material span and the caller's `cx`. It clamps resolution
independently (`min(cx, 512)` for nonzero `cx`), frames verified bounds in the
fixed isometric view, and returns a top-down premultiplied BGRA buffer
(GDI-free, so it is unit-testable without Explorer). The caller creates the DIB
section/HBITMAP and sets `WTS_ALPHATYPE`. A non-`None` result leaves `out`
empty; a fabricated success image is prohibited.

## Frozen routing table

`FamilyRouting.h` is the single source for the eight product identities, their
direct extensions, and the thumbnail-handler ShellEx GUID
`{E357FCCD-A995-4576-B01F-234630154E96}`. It matches the roster in
[05-thumbnail-provider.md](05-thumbnail-provider.md#com-classes-and-extension-assignment)
and [ADR-0001](adr/0001-eight-family-clsid-roster.md); no value may be
regenerated or renamed.

| Family | Extensions | CLSID |
| --- | --- | --- |
| glTF | `.glb`, `.gltf` | `{A592F425-EA68-4C88-BB96-020805D4BE56}` |
| STL | `.stl` | `{BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9}` |
| PLY | `.ply` | `{F4DC6119-E235-4BAC-8089-54EDD84F8492}` |
| OBJ | `.obj` | `{D4722752-C480-4D9C-BEBE-1A9B514A8846}` |
| FBX | `.fbx` | `{FBC218D4-FD2C-41DF-B168-7F3B9E53C84E}` |
| 3MF | `.3mf` | `{D8389A63-8526-454A-9892-72F3149484B9}` |
| USD | `.usd`, `.usda`, `.usdc`, `.usdz` | `{E938BC70-4C08-4446-A15D-EE31576BFB48}` |
| STEP | `.step`, `.stp` | `{6EE961AC-AC3B-4958-A898-E30523FEE79D}` |

Routing is by CLSID only: `RouteForClsid()`/`FamilyForClsid()` return
`Family::Unknown` for anything else, and T11 maps that to
`CLASS_E_CLASSNOTAVAILABLE`. There is no content sniffing and no path recovery.
T41 consumes the same header to write the machine-level registration (design
[08](08-installation-and-registration.md#thumbnail-com-registration)); a
registration test must compare the installed values against this table so the
two cannot drift. The eventual MSI adopts the same identities.

## COM core and lifetime

`thumbnail-provider/ComCore.h` (implemented in `ComCore.cpp`, T11) owns the
in-proc server's class factories and lifetime bookkeeping behind the fixed
two-symbol export surface:

- `GetClassObject(REFCLSID, REFIID, void**)` builds one `IClassFactory` per
  `FamilyRoutes()` entry; unknown CLSIDs return `CLASS_E_CLASSNOTAVAILABLE`,
  non-`IUnknown`/`IClassFactory` IIDs return `E_NOINTERFACE`, a null out-param
  returns `E_POINTER`, and aggregation returns `CLASS_E_NOAGGREGATION`.
- `ModuleLifetime` (atomic object/lock/active-call counters) plus
  `ActiveCallGuard` back `DllCanUnloadNow`, which is `S_OK` only when all three
  are zero. `RecordModuleHandle`/`ModuleHandle` are set by a side-effect-free
  `DllMain`.
- The created object is a `ProviderObject` shell implementing only `IUnknown`;
  T12 adds `IInitializeWithStream`, T13 adds `IThumbnailProvider` and T16 wraps
  `GetThumbnail` in `ActiveCallGuard`. [ADR-0013](adr/0013-provider-com-core-lifetime.md)
  records the decision.

## Shared source subset (handed to T07)

ADR-0004 compiles the needed source files directly into the DLL. T04 enumerates
the exact subset and classifies each file as **move** (relocate to a shared
format-agnostic location), **duplicate** (a behaviour-identical provider copy),
or **exclude** (intrinsically worker/viewer/IPC-coupled). T07 performs the
extraction; family tasks consume the result.

### model-core and platform (compiled into the DLL)

| File | Class | Why |
| --- | --- | --- |
| `shared/model-core/include/model_core/ImportError.h` | move | Typed error taxonomy; already format-agnostic. |
| `shared/model-core/include/model_core/MaterialPayload.h` | move | Normalized material + fallback contract. |
| `shared/model-core/include/model_core/PixelFormats.h` | move | Texture-decode contract for the glTF/FBX/OBJ/3MF adapters (T24–T32). |
| `shared/platform/include/platform/CheckedMath.h` | move | Checked integer/range helpers used at every file-derived offset. |
| `shared/model-core/include/model_core/Checksum.h` | exclude | Wire-section integrity only; the provider has no shared section. |
| `shared/model-core/include/model_core/WireFormat.h` | exclude | IPC wire layout; the provider has no IPC. |
| `shared/model-core/include/model_core/VertexLayouts.h` | exclude | Wire vertex layout; the provider uses `ProviderTypes`. |
| `shared/model-core/include/model_core/GeometryBounds.h` | exclude | `ChunkDescriptor` utility; the sampler owns bounds. |
| `shared/model-core/include/model_core/MappedFile.h`, `src/MappedFile.cpp` | exclude | Path/handle mapping; the provider reads only `IStream` (T12). |
| `shared/model-core/include/model_core/FileIdentity.h` | exclude | Path/handle identity, not needed over a stream. |
| `shared/model-core/include/model_core/ControlChannelIo.h`, `ControlProtocol.h`, `src/ControlChannelIo.cpp` | exclude | IPC channel/protocol. |
| `shared/model-core/include/model_core/OpenUsdIdentifier.h` | exclude | USD identifier helper. |
| `shared/model-core/include/ModelCore.h`, `src/ModelCore.cpp` | exclude | Stub `Version()` only. |

### `shared/parser-core/` (extracted by T07, compiled into both consumers)

T07 turned the "compile the same source files" intention into a compilable boundary by extracting the
format-agnostic parser primitives both consumers need into `shared/parser-core/`. Nothing in this
directory includes an IPC, mapping, broker, worker or viewer header; the import worker's
`StlAdapter.cpp`/`PlyAdapter.cpp` now consume it as well, so a change to a shared primitive changes
both the worker and the provider ([ADR-0004](adr/0004-share-source-not-state.md),
[ADR-0012](adr/0012-provider-parser-core-extraction.md)). The provider project compiles these three
translation units (with the provider precompiled header disabled, because the worker build has none),
and `Tests.Unit.exe` compiles them too as the `[parser]` regression set.

| File | Class | Why |
| --- | --- | --- |
| `shared/parser-core/include/parser_core/AsciiTokenizer.h`, `src/AsciiTokenizer.cpp` | move | std-only bounded tokenizer shared by ASCII STL/PLY. |
| `shared/parser-core/include/parser_core/StlParserCore.h`, `src/StlParserCore.cpp` | move | Binary-STL layout/limits, native-endian reads, per-facet validation and supplied/flat-normal policy, and ASCII/binary dialect detection. |
| `shared/parser-core/include/parser_core/PlyParserCore.h`, `src/PlyParserCore.cpp` | move | PLY scalar-type model, endian-aware scalar reads, color normalization, and the bounded header parser. |

The wire-emitting and Tier A/B streaming loops that call these primitives stay in the worker.
`StlAdapter.*`/`PlyAdapter.*` are therefore reclassified **duplicate** (a worker wire/streaming shell)
rather than move: their `BoundedChunkWriter`/`ChunkBatchSink`/`CoarseSampler`/`MappedFile` coupling is
intrinsic to the worker's IPC, progressive-delivery and coarse-proxy protocol and cannot cross into
the DLL. The provider's T21/T23 adapters instead reuse `shared/parser-core/` and emit
`IGeometrySink`/`IMaterialSink` samples. This is the "move the format-agnostic code once; duplicate
the intrinsic policy shim" rule from [ADR-0004](adr/0004-share-source-not-state.md).

### Product parser cores (from `import-worker/src`)

| File | Class | Phase | Why |
| --- | --- | --- | --- |
| `AsciiTokenizer.h`, `AsciiTokenizer.cpp` | moved | T07 | Now `shared/parser-core/`; std-only bounded tokenizer shared by ASCII STL/PLY. |
| `StlAdapter.h`, `StlAdapter.cpp` | duplicate | T21 | Worker wire/streaming shell; reuses `parser_core::StlParserCore` and stays out of the DLL. The provider copy emits `IGeometrySink` samples. |
| `PlyAdapter.h`, `PlyAdapter.cpp` | duplicate | T23 | Worker wire/streaming shell; reuses `parser_core::PlyParserCore` and stays out of the DLL. The provider copy emits `IGeometrySink`/`IMaterialSink` samples. |
| `BoundedChunkWriter.h` | exclude | — | Wire batch writer (IPC). The Tier A count constants it exposed already live in the shared `model_core/TierALimits.h` (`kTierATriangleLimit`/`kTierAPointLimit`/`kTierAVertexLimit`), which `parser_core::StlParserCore` uses directly. |
| `ChunkBatchSink.h`, `ChunkBatchSink.cpp` | exclude | — | Shared-section batch sink (IPC). |
| `CoarseSampler.h` | exclude | — | Viewer coarse-proxy sampler; the provider uses T14. |
| `SidecarFileClient.h`, `SidecarFileClient.cpp` | exclude | — | The provider resolves no external sidecars. |
| `GenerationWorker.*`, `WorkerRequestDispatch.*`, `main.cpp` | exclude | — | Worker dispatch/lifecycle. |
| `ImageFormatSniff.h`, `ImageFormatSniff.cpp` | duplicate | T25 | Byte-level image sniffing; std-only, shared by texture decoders. |
| `TextureDecodePolicy.h` | duplicate | T25 | Decoded-byte/MIME ceilings for the provider's stricter budget. |
| `WicImageDecodeAdapter.*`, `WebpDecodeAdapter.*`, `TextureTranscodeAdapter.*`, `DracoDecodeAdapter.*`, `MeshoptDecodeAdapter.*` | duplicate | T25 | Decoder cores currently emit wire chunks; provider copies emit product images/samples. ADR-0003 defines the allowed decoders. |
| `GltfAdapter.h`, `GltfAdapter.cpp` | duplicate | T25 | fastgltf core; provider copy is embedded-data-URI-only (no `SidecarFileClient`). |
| `ObjAdapter.*`, `UfbxMaterialConversion.*` | duplicate | T24 | ufbx OBJ core; provider has no MTL/texture sidecars. |
| `FbxAdapter.*` | duplicate | T31 | ufbx FBX core; embedded geometry/textures only. |
| `ThreeMfAdapter.*`, `ThreeMfOpcPreflight.*`, `ThreeMfDisplayProperties.*` | duplicate | T32 | lib3mf/OPC core under provider budgets. |
| `UsdAdapter.*`, `UsdZipPreflight.*` | duplicate | T33 | TinyUSDZ static subset; no composition, contained entries only. |
| `UsdImportWorker.*`, `UsdSpikeWorker.*`, `FbxSpikeWorker.*`, `ThreeMfSpikeWorker.*`, `ThreeMfImporterVersion.h`, `SyntheticSceneGenerator.*`, `ContainmentProbes.*` | exclude | — | Worker/spike/test-only scaffolding. |

STEP links the separately built, explicitly limited OCCT adapter (ADR-0002)
whose extraction is owned by T34; it never enters the worker or viewer.

## Follow-on tasks

- **T05** scaffolds the hardening flags, exports and test wiring for the
  provider project; headers above are already part of it.
- **T06** defines the concrete `ProviderLimits`, `Deadline` and
  `AllocationLedger` named (but not defined) by `ProviderTypes.h`, plus the
  HRESULT mapping and checked file-derived arithmetic (`ProviderLimits.h`,
  `Deadline.h`, `AllocationLedger.h`, `ProviderErrors.h`; [ADR-0011](adr/0011-provider-budgets-deadline-ledger-and-hresult.md)).
  T12/T14/T15 and adapters charge controlled allocations against
  `AllocationLedger::ProcessWide()`; the 384 MiB total-process-commit figure stays
  a measured T51 target, not a ledger claim. `Interfaces.md` consumers (T07) must
  therefore see `platform/CheckedMath.h` available in the provider build.
- **T07** performs the extraction and proves the subset compiles with no
  viewer/worker/host header.
- **T11–T17** implement routing, the COM core, the bounded source, the sampler
  and the rasterizer against these signatures.
- **T21–T34** each implement one `IFamilyAdapter`; **T41** consumes
  `FamilyRouting.h` for registration.
# Preview 3D native MVP design

Status: implementation design baseline  
Last reviewed: 2026-09-06  
Source brief: [`.docs/Start.md`](../Start.md)

This directory specifies a native Windows 11 MVP for **Preview 3D**: Explorer thumbnails plus a fast, standalone Win32/C++ Direct3D 12 viewer. The viewer is intentionally not Flutter-based. Its loading system uses memory-mapped source files, bounded background parsing, progressive mesh chunks, a fenced upload ring on a D3D12 copy queue, video-memory-budget-aware detail residency, and a bounded persistent derived-data cache for fast repeat opens. Complex USD composition is handled by a lazily started, zero-capability AppContainer compatibility host so the main viewer remains lean and failure-isolated.

The supported input families are GLB/glTF, STL, PLY, OBJ with MTL, FBX, 3MF, USD/USDZ, and a bounded static STEP/STP subset through a dedicated OCCT host. Support means the static preview subset in `03-file-formats-and-ingestion.md`, not complete authoring-tool fidelity. The glTF subset includes Draco geometry and KTX2/Basis textures; the broader USD path uses OpenUSD only in the compatibility host; the STEP/STP subset is self-contained only, because external STEP documents are out of scope by recorded product decision (ADR-017), and the OCCT host is exposed through the viewer and package (STEP-007).

## Document map

1. [`01-product-scope.md`](01-product-scope.md) — users, platform, requirements, exclusions, and release acceptance.
2. [`02-system-architecture.md`](02-system-architecture.md) — process/thread boundaries, dependencies, ownership, and data flow.
3. [`03-file-formats-and-ingestion.md`](03-file-formats-and-ingestion.md) — exact format subsets, mapped I/O, parser adapters, normalization, limits, and errors.
4. [`04-rendering-and-streaming.md`](04-rendering-and-streaming.md) — D3D12 device, queues, upload ring, synchronization, progressive LOD, residency, and device recovery.
5. [`05-thumbnail-provider.md`](05-thumbnail-provider.md) — COM implementation, stream-only import, CPU rasterization, isolation, and bitmap ownership.
6. [`06-application-lifecycle-and-ipc.md`](06-application-lifecycle-and-ipc.md) — startup, active-instance forwarding, cancellation, close, and shutdown.
7. [`07-user-experience.md`](07-user-experience.md) — window, chrome, camera, and error UX as currently implemented in the GLB vertical slice; progressive-detail/accessibility/cache UX described elsewhere remain forward targets until later gates land.
8. [`08-installation-and-registration.md`](08-installation-and-registration.md) — MSI contents, COM and application registration, signing, upgrade, and uninstall.
9. [`09-quality-performance-and-security.md`](09-quality-performance-and-security.md) — test matrix, performance method, fuzzing, threat controls, and release gates.
10. [`10-delivery-plan.md`](10-delivery-plan.md) — gated implementation slices, traceability, and definition of done.
11. [`11-decisions-and-risks.md`](11-decisions-and-risks.md) — accepted decisions, known risks, triggers, and fallback choices.

## Precedence

`MUST`, `SHOULD`, and `MAY` are normative requirements terms. If documents conflict, precedence is:

1. `01-product-scope.md` for product commitments;
2. `03-file-formats-and-ingestion.md` for accepted content and CPU limits;
3. `04-rendering-and-streaming.md` for GPU lifetime/synchronization;
4. the relevant component design;
5. `10-delivery-plan.md` for sequence only;
6. `Start.md` for original product intent.

Changing scope requires updating affected requirement IDs, format matrices, tests, risks, and the delivery traceability table together.

## Corrections and refinements to the pivot advice

The recommended direction is retained, with these engineering clarifications:

- A read-only file mapping avoids an eager full-file userspace copy. It reserves virtual address space and faults pages through the OS cache as accessed; parsing still allocates metadata and normalized vertex/index data. Mapping does not by itself prevent out-of-memory conditions.
- Source mapping is not a direct GPU upload. Data that needs validation, deinterleaving, coordinate conversion, normal generation, or decompression is written into a persistently mapped upload heap and then copied to default-heap GPU resources.
- A copy queue may overlap a discrete GPU's copy engine with graphics, but overlap is hardware/driver/workload dependent and differs on integrated/UMA adapters. The design measures it rather than promising PCIe/DMA timing.
- The render queue does not wait on an unfinished chunk and stall the frame. The upload coordinator publishes a chunk only after its copy fence is complete. Cross-queue waits are reserved for rare batched transitions where publishing after CPU fence observation is not viable.
- A bounding sphere is immediately available only when trustworthy metadata contains bounds. glTF accessor min/max gives a provisional bound; STL and many other formats require a scan. Provisional bounds are verified before final camera fit.
- A multi-gigabyte source can exceed available VRAM even though mapping succeeds. The viewer therefore retains a small always-resident proxy and pages fine-detail chunks under a live DXGI video-memory budget. “Upload the whole file” is not the large-asset strategy.
- `fastgltf` covers glTF 2.0 structure; product adapters add the pinned Draco decoder, KTX2/Basis transcoder, and texture decoders. STL and PLY use product-owned bounded parsers; ufbx covers FBX and OBJ/MTL; lib3mf covers 3MF. TinyUSDZ is the fast USD/USDZ path and an AppContainer OpenUSD host is the compatibility fallback.
- A persistent cache improves repeat opens but is never trusted as source data. Entries are versioned, checksummed, identity-bound, size-limited, LRU-evicted, and clearable by the user. A miss or corrupt entry falls back to ordinary import.
- 144 Hz is a display-dependent operating point, not a universal promise. Loading work is prohibited from blocking input/present, and measured frame-time gates are defined for a 144 Hz reference system.

## Ingestion security refactor (external review, 2026-09-07)

An external security review of the ingestion/rendering design (full findings recorded as [ADR-014](11-decisions-and-risks.md#adr-014-appcontainer-import-processes-are-the-parser-security-boundary-not-threads)) reached this verdict: the renderer design is solid, but background threads are a scheduling boundary, not a security boundary, and every source parser — not only OpenUSD — must run in a zero-capability AppContainer process before broad format support ships. Consequences applied throughout this document set:

- a new general-purpose `Preview3DImportWorker.exe` now hosts every fast-path parser/decoder (fastgltf, STL/PLY, ufbx, lib3mf, TinyUSDZ, Draco, KTX/Basis, libwebp, DirectXTex/WIC, meshoptimizer); `Preview3D.exe` links none of them ([02](02-system-architecture.md), [03](03-file-formats-and-ingestion.md), [11](11-decisions-and-risks.md) ADR-014);
- a shared section is validated by copying it to private host memory before trust, not merely by inspecting it in place, because a compromised worker retains write access to a mapped section for as long as it stays mapped ([02](02-system-architecture.md), [03](03-file-formats-and-ingestion.md));
- the renderer's descriptor-table baseline, D3D12MA multi-heap use, per-frame CBV camera data, and enumerated vertex layouts are confirmed and locked as invariants rather than revised ([04](04-rendering-and-streaming.md), [11](11-decisions-and-risks.md) ADR-009);
- the thumbnail handler's isolation now explicitly forbids `DisableProcessIsolation` ([05](05-thumbnail-provider.md), [11](11-decisions-and-risks.md) ADR-007);
- the import sandbox and its hostile-worker proof move to Gate 2, before any real parser is wired to it ([10](10-delivery-plan.md)).

Separately, [`07-user-experience.md`](07-user-experience.md) was rewritten to describe the UI/UX actually implemented in the current GLB vertical slice (custom chrome, flight/orbit camera, navigation gizmo, info panel) rather than the earlier aspirational multi-format UX description; see that document's scope note.

## Primary technical references

- [Microsoft: creating file mappings](https://learn.microsoft.com/en-us/windows/win32/memory/creating-a-file-mapping-object)
- [Microsoft: fence-based resource management and upload rings](https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management)
- [Microsoft: D3D12 resource barriers and queue states](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12)
- [Microsoft: D3D12 residency and video-memory budgets](https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency)
- [Microsoft: explicit synchronization in D3D12](https://learn.microsoft.com/en-us/windows/win32/direct3d12/important-changes-from-directx-11-to-directx-12)
- [fastgltf repository](https://github.com/spnda/fastgltf)
- [Google Draco repository](https://github.com/google/draco)
- [Khronos KTX-Software repository](https://github.com/KhronosGroup/KTX-Software)
- [ufbx repository](https://github.com/ufbx/ufbx)
- [lib3mf repository](https://github.com/3MFConsortium/lib3mf)
- [TinyUSDZ repository](https://github.com/lighttransport/tinyusdz)
- [OpenUSD documentation](https://openusd.org/release/)
- [Microsoft DirectXTex repository](https://github.com/microsoft/DirectXTex)
- [meshoptimizer repository](https://github.com/zeux/meshoptimizer)
- [D3D12 Memory Allocator repository](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator)
- [Khronos glTF 2.0 specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)
- [OpenUSD USDZ specification](https://openusd.org/dev/spec_usdz.html)
- [Microsoft: `IThumbnailProvider`](https://learn.microsoft.com/en-us/windows/win32/api/thumbcache/nn-thumbcache-ithumbnailprovider)
- [Microsoft: Windows app defaults](https://learn.microsoft.com/en-us/windows/apps/develop/windows-integration/default-apps-platform)

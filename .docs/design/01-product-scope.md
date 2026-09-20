# Product scope

## Product statement

3D Preview helps artists, 3D-printing users, and developers identify local model assets in Explorer and inspect one without opening a full DCC/CAD application. The product prioritizes immediate window/input response, progressive visual feedback, bounded memory, and safe failure over editing features or exact authoring-renderer parity.

The two capabilities are:

- model-derived Explorer thumbnails produced by an isolated native COM handler;
- a standalone native viewer with orbit/pan/zoom and progressive detail.

The Explorer Preview pane (`IPreviewHandler`) and a global spacebar/Quick Look hook are not included.

## Supported environment

| Dimension | MVP commitment |
| --- | --- |
| OS | Serviced Windows 11 releases |
| CPU architecture | x64 only |
| Viewer graphics | Direct3D 12 adapter supporting feature level 11_0 or later; WARP is diagnostic fallback, not a performance target |
| Display | DPI-aware windowed desktop; 60–144 Hz tested |
| Storage | Local NTFS/ReFS source paths; best-effort per-user LocalAppData derived cache; source may be on slower media but performance targets use NVMe |
| Distribution | Original MVP: signed per-machine MSI. Scope-limited MVP exception: signed, checksummed portable x64 ZIP per ADR-016. |
| Connectivity | No product network access or remote-resource resolution |
| Locale | English UI; Unicode file paths fully supported |

Windows 10, ARM64/x86, Store/MSIX packaging, server/headless sessions, and UNC/device paths are not supported targets. Portable ZIP is supported only for the scope-limited MVP exception in ADR-016; it does not provide the original MSI/shell feature set.

## Functional requirements

| ID | Requirement |
| --- | --- |
| FR-01 | The product MUST recognize `.glb`, `.gltf`, `.stl`, `.ply`, `.obj`, `.fbx`, `.3mf`, `.usd`, `.usda`, `.usdc`, and `.usdz` case-insensitively. `.mtl` is an OBJ sidecar, not a directly opened type. |
| FR-02 | Explorer MUST receive a model-derived thumbnail for valid files within the provider's stream-contained feature and resource limits. Corrupt, unsupported, sidecar-dependent, or over-budget input MUST fail safely so Explorer can use its normal icon. |
| FR-03 | The viewer MUST open a supported path supplied by command line/file association, Open With, an open dialog, or one-file drag-and-drop. |
| FR-04 | The native window and first background frame MUST appear before model parsing or GPU upload. Input, resize, and painting MUST remain responsive throughout loading. |
| FR-05 | File reads, cache validation, parsing, normalization, texture decode/transcode, LOD construction, compatibility-host IPC, default-heap allocation, and copy recording/submission MUST not execute on the Win32 UI thread. Parsing, normalization, and decode/transcode of untrusted source bytes MUST additionally execute only inside the AppContainer import process required by NFR-14, not merely on a background thread of the trusted viewer process. |
| FR-06 | The viewer MUST support orbit, pan, dolly/zoom, fit-to-view, and reset camera through mouse and keyboard. |
| FR-07 | Supported geometry MUST become visible progressively: an always-resident proxy or early complete chunks first, then higher-detail fence-complete chunks. The app MUST never draw a partially copied resource. |
| FR-08 | Assets larger than the current viewer VRAM target MUST remain inspectable through proxy plus view-prioritized fine-detail residency rather than failing solely because all detail cannot be resident simultaneously. |
| FR-09 | A visible instance MUST accept a later supported activation and cancel/replace older work safely. When no instance is running, startup is cold and native; closing the window terminates the process. |
| FR-10 | Loading failure MUST produce an actionable in-window error without hanging the message pump. A subsequent valid activation MUST recover without relaunch when the window remains open. |
| FR-11 | The viewer MUST render static geometry with depth, generated or source normals, studio lighting, vertex color, and the documented material/texture subset. The glTF path MUST decode supported Draco meshes and KTX2/Basis textures. FBX MUST evaluate its documented static deformation pose. STL uses a neutral material. |
| FR-12 | The installer MUST register 3D Preview as an available handler for every direct extension without overwriting the user's current default-app choices. |
| FR-13 | Install, repair, upgrade, and uninstall MUST leave Explorer and existing file associations usable and MUST not normally require a reboot. |
| FR-14 | The viewer MUST maintain an optional, bounded per-user derived-data cache containing verified coarse proxies and reusable normalized resources. It MUST provide a keyboard-accessible Clear cached previews command and MUST behave correctly when the cache is absent, stale, corrupt, full, or disabled. |
| FR-15 | USD files outside the TinyUSDZ fast subset MUST be retried through a lazily started native compatibility host using the same static-scene contract and local-resource policy. Host failure MUST become a recoverable document error and MUST not terminate or stall the viewer. |

## Non-functional requirements

| ID | Requirement |
| --- | --- |
| NFR-01 UI isolation | The UI thread MUST only own HWND/message/input/application-state work. No task it originates during loading may synchronously wait on disk, a worker, a GPU fence, or device idle. |
| NFR-02 Render isolation | A dedicated render thread MUST own direct-queue submission, swap-chain resize, and `Present`. Loader/upload failure cannot stop it from clearing and presenting the UI scene. |
| NFR-03 Startup | On reference hardware, a cold zero-argument launch MUST present the app background within 150 ms at p95 and a file launch MUST present loading UI within 200 ms at p95. |
| NFR-04 Loading frames | On the 144 Hz reference display, the empty/loading UI MUST have p95 CPU-to-present frame interval ≤8.3 ms and no load-caused interval above 50 ms. Vsync/display scheduling exceptions are recorded separately. |
| NFR-05 First geometry | Small input MUST show geometry within 500 ms p95, medium within 2 s p95, and the Tier-A multi-gigabyte fixture MUST show a meaningful proxy/partial model within 5 s p95. |
| NFR-06 Interaction | While loading, p95 pointer-event-to-affected-present latency MUST be ≤16 ms on the performance reference and ≤33 ms on the compatibility reference. Once ready, small/medium scenes MUST sustain 60 fps and the large proxy/resident-detail scene at least 30 fps on their applicable reference systems. |
| NFR-07 Memory | Tier-A ingestion MUST not copy the entire source into a private heap. CPU scratch, transient normalized data, persistent cache, upload, compatibility-host, and GPU allocations MUST be explicitly budgeted; allocation failure becomes a controlled degraded state/error. |
| NFR-08 Residency | Viewer-owned local-video-memory target MUST adapt to `QueryVideoMemoryInfo`, remain below the policy cap in `04-rendering-and-streaming.md`, and shed fine detail when the OS budget drops. |
| NFR-09 Thumbnail stability | No valid or malformed regression-corpus item may crash or hang Explorer. The provider MUST use `IInitializeWithStream`, retain Shell isolation, and do no UI/network/process launch/write. |
| NFR-10 Accessibility | All non-canvas controls MUST be keyboard reachable and expose name/role/state. The UI MUST remain usable at 200% scale and in Windows high contrast. |
| NFR-11 Privacy | The MVP MUST contain no analytics, account, cloud upload, advertising, auto-update, or product-initiated network fetch. Diagnostics are local and opt-in. The derived-data cache is local-only, contains no source path or user-visible names, and is the only persistent model-derived store owned by the viewer. |
| NFR-12 Reproducibility | Toolchain, Agility SDK if used, shaders, parsers, decoders/transcoders, OpenUSD compatibility payload, allocator, and all transitive dependencies MUST be pinned and represented in the release SBOM. |
| NFR-13 Cache safety | A cache hit MUST validate schema/importer versions, reliable source/dependency version evidence or a full content digest, expected lengths, and cache-section checksums before publication. Last-write time alone is insufficient. The default global soft cap is 10 GiB and per-entry cap is 2 GiB; eviction MUST begin earlier when free-space policy requires it. Invalid cache data is a miss, never a crash or partially trusted scene. |
| NFR-14 Import isolation | Every viewer-side source parser and decoder — fastgltf, the STL/PLY product parsers, ufbx, lib3mf, TinyUSDZ, the Draco decoder, the KTX/Basis transcoder, libwebp, DirectXTex/WIC, OpenUSD, and any future compatibility importer — MUST run only inside an AppContainer import process (the general import worker or, for OpenUSD, the compatibility host) under a zero-capability token and process/job memory/time limits, launched suspended or job-assigned before it processes any input, with brokered read-only source access via an explicit, minimal inherited-handle list, no network capability, and no environment/model-selected plug-in loading. Executing a parser on a background thread of the trusted viewer process satisfies FR-04/FR-05's responsiveness requirement but never satisfies this requirement by itself. |

Performance values are controlled-hardware service objectives, not claims for every device or model. Correctness and input responsiveness remain mandatory on slower supported systems.

## Workload classes and support tiers

| Class | Source size | Geometry | Purpose |
| --- | ---: | ---: | --- |
| Small | 1–10 MiB | ≤100k triangles, ≤4 2K textures | ordinary part/prop |
| Medium | 100–500 MiB | ≤5M triangles, ≤16 4K textures | detailed scan/assembly |
| Large Tier A | 2–4 GiB | 40–80M triangles or points, textures optional | out-of-core GLB/binary-STL/binary-PLY proof |
| Warm cache | small, medium, and large Tier-A fixtures with verified derived entries | same as source fixture | repeat-open latency and cache validation |
| Pressure | medium asset while external apps reduce DXGI budget | variable | residency shedding/recovery |
| Over-limit | exceeds a format hard limit | any | fast controlled rejection |

Tier A is GLB, glTF with binary sidecars, binary STL, and supported binary PLY layouts: these have offset-oriented data suitable for mapped, chunked access. Tier B is ASCII STL/PLY, OBJ/MTL, FBX, 3MF, USD-family data, and the bounded static STEP/STP subset: they are supported within their lower limits, remain off the UI/render threads, and load progressively after their parser can emit normalized batches, but multi-gigabyte performance is not promised for them. OpenUSD-backed USD and STEP/STP remain Tier B even though they run out of process.

## Explicit exclusions

The MVP does not include:

- editing, repair, slicing, export/conversion, measurement, annotation, picking, or selection;
- animation playback, simulation, skeletal posing, morph controls, material variants UI, scene/camera/light selection;
- CAD kernels or unlisted formats such as IGES, IFC, AMF, DAE, and native Blender files. A bounded static STEP/STP preview subset through the dedicated OCCT host is designed separately and is not a general CAD kernel; it is self-contained only, because external STEP documents are out of scope by recorded product decision (see ADR-017);
- ray tracing, mesh-shader-only rendering, GPU decompression, DirectStorage, virtual-texture reserved resources, or multi-GPU rendering;
- perfect/full-resolution simultaneous residency for a scene larger than the safe GPU budget;
- online/UNC resource resolution, recent-files history, crash upload, automatic update, localization, background daemon, tray icon, or start-at-login;
- Explorer Preview pane, property handler, context-menu extension, or global hotkey;
- Windows 10/ARM64/x86/Store support.

## Release acceptance

The MVP is releasable when:

1. all `FR-*` requirements pass on clean Windows 11 x64 systems;
2. all `NFR-*` gates have raw evidence for the signed Release build;
3. Tier-A 2–4 GiB GLB, binary STL, and supported binary PLY fixtures load without full-source heap copies, message/render stalls, or GPU-budget overcommit;
4. every Tier-B family passes its positive, negative, sidecar, and limit corpus;
5. copy/direct queue debug-layer and GPU-based validation runs report no lifetime, state, or synchronization error;
6. install/upgrade/repair/uninstall leaves no product COM or association orphan and no normal-case reboot;
7. native fuzz/sanitizer corpora produce no known crash, hang, out-of-bounds access, or unbounded allocation;
8. signed binaries, SBOM, dependency notices, symbols, checksums, format limitations, and performance evidence are archived together;
9. cold and warm-cache performance gates pass, cache corruption/staleness behaves as a miss, and Clear cached previews removes the current user's derived entries;
10. both AppContainer import processes — the general import worker covering every format in the support matrix and the OpenUSD compatibility host — pass broker/path/network denial, memory/time limit, crash, cancellation, malformed-input/malformed-stage, and hostile-worker (mutated shared-section) tests without compromising the viewer.

# System architecture

## Runtime topology

The installed product has four runtime components: the trusted native viewer EXE, a zero-capability AppContainer import-worker EXE, a lazily started AppContainer OpenUSD compatibility-host EXE, and the Explorer thumbnail COM DLL. The viewer links none of the third-party format-parsing or decoding libraries. Every parser and decoder — fastgltf, the STL/PLY product parsers, ufbx, lib3mf, TinyUSDZ, the pinned Draco decoder, the KTX/Basis transcoder, libwebp, and DirectXTex/WIC — runs only inside `Preview3DImportWorker.exe`. `Preview3DImportHost.exe` additionally links OpenUSD and its pinned app-local support payload for USD composition outside the TinyUSDZ subset. The thumbnail provider links its own copy of the bounded fast-path parsers for in-process CPU sampling under Shell's separate process isolation; it shares no process, memory, or cache with the viewer or either import process.

```text
Explorer thumbnail host                         Preview3D.exe (trusted)
        |                                           |
        | IStream                                   +-- Win32 UI thread
        v                                           |     message/input/state only
Preview3DThumbnailProvider.dll                      |
        | stream-buffer adapters                    +-- Render thread
        v                                           |     direct queue/swap chain/present
model-core parsers -> CPU rasterizer                |
                                                    +-- Loader/broker pool
                                                    |     open/canonicalize, cache validate,
                                                    |     dispatch, snapshot+validate chunks
                                                    |
                                                    +-- Upload coordinator
                                                          upload ring/copy queue/fence
                                                                 |
                                                     completed, host-validated chunks only
                                                                 v
                                                      render scene/residency manager

Preview3D.exe -- brokered read-only handles + shared sections --> Preview3DImportWorker.exe
                                                                   zero-capability AppContainer
                                                                   fastgltf / STL / PLY / ufbx /
                                                                   lib3mf / TinyUSDZ / Draco /
                                                                   KTX-Basis / WebP / DirectXTex-WIC
                                                                   normalization into bounded
                                                                   output chunks

Preview3D.exe -- brokered read-only handles + shared sections --> Preview3DImportHost.exe
                                                                   zero-capability AppContainer
                                                                   pinned OpenUSD, broader
                                                                   local composition
                                                                   bounded normalized chunks

Preview3D.exe -- brokered read-only handles + shared sections --> Preview3DStepHost.exe
                                                                   zero-capability AppContainer
                                                                   pinned OCCT, ISO 10303-21
                                                                   admission + XDE traversal
                                                                   bounded normalized chunks
```

There is no runtime IPC between the thumbnail DLL and any of the product executables. The provider never loads or launches the viewer, the import worker, either import host, or the STEP host. The viewer contains no COM thumbnail object. `Preview3DImportWorker.exe` starts at the first Open of a session and is reused, under a fresh job/sandbox, across later generations; `Preview3DImportHost.exe` starts only for a USD generation that exceeds the TinyUSDZ fast subset and exits after that generation or an idle grace period; `Preview3DStepHost.exe` starts only for a STEP/STP generation and exits after that generation (or a short measured idle grace), discarding OCCT global state and peak B-rep memory deterministically. No import process is ever a daemon, a Shell child, or has UI, a GPU device, or path/network authority. The STEP host and its OCCT payload are not in the viewer, general worker, or thumbnail closure.

Background worker threads inside `Preview3D.exe` (the loader pool, upload coordinator, cache coordinator) are a **scheduling boundary**: they keep parsing and I/O off the UI/render threads so the app stays responsive. They are not a **security boundary** — a memory-corruption bug in a parser running on a background thread still compromises the trusted process that owns the window, the D3D12 device, and the user's other open documents. The AppContainer import processes are the security boundary; see [ADR-014](./11-decisions-and-risks.md#adr-014-appcontainer-import-processes-are-the-parser-security-boundary-not-threads).

## Execution lanes

### Win32 UI thread

The `wWinMain` thread initializes COM as STA, establishes singleton ownership, starts the IPC service, creates/shows the top-level HWND, and runs `GetMessage`. It owns:

- window lifetime, non-client hit testing, DPI/theme/accessibility notifications;
- immutable input snapshots and high-level app/load state;
- open dialog, drag/drop admission, keyboard commands, and error/setting commands;
- `PostMessage`/bounded-queue dispatch to other lanes.

It never parses, maps large asset regions, decodes textures, records/submits D3D12 commands, calls `Present`, waits for a fence, or joins a load thread during normal operation.

### Render thread

One dedicated thread initializes DXGI/D3D12, owns the direct command queue, swap chain, frame contexts, depth/render targets, graphics PSOs/root signatures, draw-list consumption, resize, and `Present`. It renders from immutable scene snapshots published at frame boundaries. It polls completed copy/direct fences without blocking the UI.

Only this thread mutates the active render scene and descriptor-visible draw records. GPU resources use deferred release tagged with the last direct-fence value that can reference them.

### Loader/broker pool

A bounded `std::jthread` pool in `Preview3D.exe` no longer runs third-party parser code. Pool width is calculated with saturating arithmetic from available logical processors, initially leaving at least two logical processors outside the pool and capped at eight, and can be lowered when frame latency, input latency, power state, or memory pressure crosses policy thresholds. Pool work runs below UI/render priority and performs only:

- opening and canonicalizing the primary file and approved local dependencies (§ Input boundary in [03-file-formats-and-ingestion.md](./03-file-formats-and-ingestion.md));
- a trivial extension/magic-number sniff, bounded to the first few dozen bytes, used only to pick which import-worker job type to request — no structural parsing happens here;
- deriving and validating the persistent-cache key and manifest;
- dispatching an import job to the broker (below) and receiving its typed result;
- **copying and validating** every chunk descriptor and its bytes returned by an import process before it is eligible for upload (see Import worker/host below) — this step, not the process boundary alone, is what makes a chunk trustworthy;
- assembling verified scene-wide bounds from validated chunks and admitting a validated cache write.

Every load has a generation and `stop_source`. Work checks cancellation between bounded units. Results go through bounded queues; workers may backpressure, never allocate an unbounded result backlog.

### Import worker

`Preview3DImportWorker.exe` is where fastgltf, the STL/PLY product parsers, ufbx, lib3mf, TinyUSDZ, the pinned Draco decoder, the KTX/Basis transcoder, libwebp, DirectXTex/WIC, and meshoptimizer actually run against untrusted bytes. It performs the metadata/dependency preflight, structural parsing/validation, normalization into the chunk contract in [03-file-formats-and-ingestion.md](./03-file-formats-and-ingestion.md), texture decode/transcode/mip generation, and meshoptimizer-based proxy/LOD construction — everything the loader pool used to do in-process. TinyUSDZ's common-subset attempt also runs here; only its typed `UnsupportedComposition` result asks the broker to start the heavier OpenUSD host instead, for that one generation.

The worker runs under a zero-capability AppContainer token, assigned to a kill-on-close Job Object with commit, process-count, CPU-time, and child-process limits *before* it processes any input: the broker launches it suspended (or assigns the job at process-creation time) and resumes it only after the token, job, and handle list are in place, so no worker code executes with fewer restrictions than its final policy. The AppContainer SID has read/execute access only to the installed worker payload and explicit access to per-generation broker objects; it has no network capability, no inherited directory handle, no direct model-directory access, no current-directory search, and no environment- or model-selected plug-in path. Only the private control-channel pipe, the duplicated read-only source/dependency handles, and the shared-section handles for the current generation are inherited, passed explicitly through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` rather than by broad handle inheritance.

The worker starts at the first Open in a session and is reused, under a fresh AppContainer token/Job Object pairing, for later generations rather than restarted per file — restarting per file is a fallback the broker may use after a crash or limit violation, not the steady-state behavior. It never receives a path string: the parent opens and canonicalizes the primary file and every approved local dependency, then duplicates read-only handles across the boundary. A product-owned asset resolver inside the worker (used for OBJ/MTL siblings, glTF buffer/image URIs, 3MF/USDZ archive entries, and TinyUSDZ composition) requests dependencies from the broker by relative reference only; arbitrary filesystem opens and URI schemes are denied on the broker side, which is the only side with path authority.

Normalized metadata and chunk bytes return through bounded shared-memory sections. **A shared section is not itself a trust boundary**: Windows gives every mapping view of the same section coherent, live access, so a compromised worker retains write access to a section for as long as it stays mapped on either side, even after the host has inspected it once. The loader/broker pool therefore treats a chunk as untrusted right up until it has been copied: it copies each chunk's descriptor (offsets, counts, strides, layout ID, generation) into private host memory, bounds-checks every field against the section length and the generation's budgets, and only then copies the referenced payload bytes into host-owned memory or directly into the upload ring. Nothing downstream — cache write, upload, or render — reads the shared section again after that copy. Worker/host objects and pointers never cross the process boundary; only the wire-format descriptors defined in [03-file-formats-and-ingestion.md](./03-file-formats-and-ingestion.md) do.

The worker is not on the window-startup path. Cancellation closes generation channels and Job Object handles after a short cooperative grace period. A crash, timeout, protocol violation, or limit terminates only the worker, invalidates its shared sections, and produces a recoverable typed import error for that generation; the broker may create a fresh worker instance for a later generation but does not retry the same failing stage in a loop.

### Compatibility host

`Preview3DImportHost.exe` handles USD composition outside the TinyUSDZ common subset. It reuses the same broker protocol, wire schema, shared-section validation rule, AppContainer/Job Object launch sequence, and handle-inheritance policy as the import worker — it is a separate executable because it additionally links pinned OpenUSD and its app-local support payload, which is materially heavier than the worker's fast-path adapters and is only needed for a minority of USD documents. Everything in the Import worker section above about copy-before-trust, suspended launch, restricted handle inheritance, and no path/network authority applies identically here.

The host's OpenUSD asset resolver requests dependencies from the broker the same way the worker's resolver does; arbitrary filesystem opens and URI schemes are denied. OpenUSD objects and pointers never cross the process boundary — only normalized chunk descriptors and their validated shared bytes do.

The host is not on the window-startup path. It starts only for the current USD generation and exits after that generation or an idle grace period; it is never a daemon. A crash, timeout, protocol violation, or limit terminates only the host, invalidates its shared sections, and produces a recoverable typed import error. The viewer may create a fresh host for a later generation but does not retry the same failing stage in a loop.

### STEP host

`Preview3DStepHost.exe` handles the bounded static STEP/STP preview subset through pinned Open CASCADE Technology. It is a fifth runtime component and reuses the same broker protocol, wire schema, shared-section validation rule, AppContainer/Job Object launch sequence, and handle-inheritance policy as the other import processes. It is separate because OCCT is materially larger and more stateful than the fast-path adapters and its transfer/tessellation phases do not offer the fine-grained cancellation of the product-owned parsers. A product-owned `StepPart21Preflight` verifies the ISO 10303-21 physical envelope, bounded structure, and lexical/entity/reference/depth/byte ceilings before the OCCT reader is reachable; only normalized chunk descriptors and their validated bytes cross the boundary. No OCCT object, path, or diagnostic string does.

The host receives an already-open read-only handle, never a path, and can load no model-selected plug-in, codec, resource, or child process. It starts only for the current STEP generation and exits after that generation; it is never a daemon. A crash, timeout, protocol violation, or limit terminates only the host, invalidates its shared sections, and produces a recoverable typed import error. Product extension discovery, packaging, and registration remain disabled until STEP-003 through STEP-006 land.

### Upload coordinator

One dedicated thread is the sole owner of copy-queue submission and upload-ring suballocation. Workers enqueue immutable `UploadBatch` descriptions plus CPU spans whose lifetime is explicit. The coordinator:

1. polls/reclaims ring ranges by copy-fence value;
2. waits only on its own event when the ring is full (UI/render continue);
3. copies normalized bytes into the persistent upload mapping;
4. records a fresh/reset copy command list and submits it serially;
5. signals a monotonically increasing copy fence;
6. moves the batch to a pending-completion queue;
7. publishes `ResidentChunk` only after `GetCompletedValue` reaches the batch fence.

Command allocators/lists are never reset until their fence is complete and are never recorded concurrently by two threads.

## Thread communication and ownership

| Payload | Producer → consumer | Mechanism | Ownership rule |
| --- | --- | --- | --- |
| Input/window snapshot | UI → render | double-buffered atomic index | render reads immutable copy |
| Load request/cancel | UI → loader coordinator | bounded MPSC queue + stop token | generation owns job graph |
| Bounds/progress/error | workers → UI | bounded queue + one coalescing `PostMessage` | small value objects only |
| Import request | loader broker → import worker / compatibility host / STEP host | private authenticated pipe + duplicated read-only handles, launched suspended/job-assigned before input processing | one current generation; no path authority in the child; same protocol for every process |
| Import normalized chunk | import worker / compatibility host / STEP host → loader broker | bounded shared section + control message | parent copies descriptor and bytes to private memory and validates header/ranges/checksum/generation before accepting them; the shared section is never trusted or re-read afterward |
| Derived cache lookup/write | loader → cache coordinator | background file I/O + atomic manifest replacement | cache data is untrusted, GPU-independent, and generation/version bound |
| Normalized upload batch | workers → upload | bounded MPSC queue | CPU backing through ring memcpy; ring range through copy fence |
| Fence-complete chunk | upload → render | bounded SPSC queue | render assumes GPU copy complete |
| Residency request/eviction | render → loader/upload | bounded priority queue | scene/chunk ID, never raw UI pointer |
| Scene snapshot | render-owned scene builder → render | atomic `shared_ptr<const ...>` swap at frame boundary | immutable; deferred GPU deletion |

No queue stores references into a remappable file view beyond the mapping lease lifetime. Raw HWND access is confined to UI/render integration code; workers post IDs/value objects.

## Components

| Component | Responsibility |
| --- | --- |
| `platform` | RAII handles/mappings, checked math, paths, clocks, thread naming, errors |
| `app` | startup, state machine, command routing, active-instance IPC, cancellation |
| `model-core` | normalized scene/chunk schema and wire-format contracts shared by the viewer and every import process; the format-sniffing magic-number table used by the viewer's dispatch sniff; the actual parser adapters and resource resolver, which link only into `import-worker`/`import-host`/`step-host` |
| `streaming` | source leases, persistent derived cache, chunk-descriptor validation/copy, view-priority residency requests |
| `import-worker` | AppContainer general-format (glTF/STL/PLY/OBJ/FBX/3MF/TinyUSDZ) parsing, decode/transcode, and normalized shared-section production |
| `import-host` | AppContainer OpenUSD stage composition and normalized shared-section production |
| `step-host` | AppContainer ISO 10303-21 admission, OCCT XDE traversal, and bounded tessellation into normalized shared-section production |
| `graphics` | adapter/device, queues/fences, D3D12MA, upload ring, descriptors, renderer, DRED |
| `ui` | custom window chrome, DirectWrite text/glyphs, overlays, accessibility providers |
| `thumbnail` | COM class factory/provider, stream adapter, bounded import, CPU rasterizer |
| `installer` | MSI files, COM/Shell/app registration, signing and lifecycle |

## Dependency choices

All versions/revisions are exact-pinned in Gate 0 and may change only through dependency review; no floating branch is a release input.

| Dependency | Use | Boundary |
| --- | --- | --- |
| fastgltf | GLB/glTF 2.0 JSON/metadata/accessors | AppContainer import-worker mapped adapter; separate provider memory-buffer adapter linked only into the thumbnail DLL |
| Google Draco decoder | `KHR_draco_mesh_compression` | bounded cancellable decode jobs inside the import worker; no encoder in runtime |
| KTX-Software/Basis transcoder | KTX2 and `KHR_texture_basisu` | bounded import-worker transcode to supported BC/RGBA formats |
| libwebp | deterministic WebP decode | import-worker/provider memory-buffer adapter where format policy permits |
| ufbx | FBX plus OBJ/MTL | AppContainer import-worker adapter with its allocation/progress limits enabled |
| lib3mf | 3MF package/Core model | AppContainer import-worker adapter; package/expanded limits applied before/through reader |
| TinyUSDZ | USDA/USDC/USDZ common static subset | AppContainer import-worker fast adapter with production build and memory budget |
| OpenUSD | broader local static USD composition | AppContainer compatibility host only; custom brokered resolver and pinned signed payload |
| Open CASCADE Technology (OCCT) | ISO 10303-21 STEP/STP transfer, XDE traversal, and B-rep tessellation | AppContainer STEP host only; constrained module closure and pinned signed payload; never the viewer, general worker, or thumbnail provider |
| meshoptimizer | vertex-cache optimization, chunk meshlets/clusters, simplification/proxy LODs | AppContainer import-worker only, over bounded normalized chunks; experimental APIs excluded unless separately gated |
| D3D12 Memory Allocator | placed-resource/default-heap allocation and budget statistics | graphics module only, inside the trusted viewer |
| DirectXTex/WIC | PNG/JPEG/BMP/TIFF/HDR/TGA/DDS decode, resize, mip generation, supported GPU formats | AppContainer import-worker; inbox WIC codecs only, no runtime-installed codec discovery |
| DirectXMath | camera/transforms/bounds | product math layer, trusted viewer |
| DXC | offline HLSL → DXIL | build-time only; compiled shader blobs embedded/packaged |
| WIL/WRL | Win32/COM resource ownership | no exception crosses COM/Win32 callbacks |

The trusted viewer links no third-party format parser or decoder — not fastgltf, Draco, KTX/Basis, libwebp, ufbx, lib3mf, TinyUSDZ, meshoptimizer, or DirectXTex/WIC — and links no Assimp, Flutter, Chromium, scripting runtime, OpenUSD, or FBX SDK either. Those parser/decoder libraries link only into `Preview3DImportWorker.exe`; OpenUSD links only into `Preview3DImportHost.exe`. The thumbnail DLL links its own separate copies of the bounded fast-path parsers for in-process CPU sampling under Shell's isolation and shares no binary state with the viewer or either import process. Neither import process can load arbitrary plug-ins or expose library-native objects to the viewer. Format adapters expose product-owned data types so third-party structures never reach graphics/UI interfaces, and cross the process boundary only as the wire-format descriptors in [03-file-formats-and-ingestion.md](./03-file-formats-and-ingestion.md).

## End-to-end viewer flow

1. UI validates extension/path and posts `OpenModel(generation, path)`.
2. The trusted loader/broker pool opens the primary file and its approved local dependencies read-only with a stable share policy, canonicalizes each handle, and records file identity/size. It performs only the trivial extension/magic-number sniff described above — no read-only file mapping of source bytes into the trusted process's address space happens here, because structural access to source bytes now happens only inside the import process.
3. Cache coordinator derives an opaque key from the primary and dependency version evidence and validates any candidate manifest, schema/importer versions, lengths, and checksums. A fast hit requires stable local file/change-journal tokens; otherwise a full content digest is required or the candidate becomes a miss. A valid, revalidated coarse proxy may publish through the normal upload path while optional cached detail is admitted by view priority.
4. On a cache miss, the broker starts or reuses `Preview3DImportWorker.exe` under its AppContainer token and Job Object, and forwards duplicated read-only handles for the primary and dependencies plus the generation's budgets over the private control channel. No path string crosses the boundary.
5. Inside the worker, the fast adapter performs the metadata/dependency preflight, creates its own read-only mapping of the handles it was given, and parses into bounded normalized chunks. Full-source heap materialization is forbidden for Tier A. Binary STL and supported binary PLY layouts use sequential windows/prefetch; sparse glTF keeps random/windowed access. TinyUSDZ first attempts the documented common USD subset; a typed `UnsupportedComposition` result asks the broker to start `Preview3DImportHost.exe` for that generation instead — malformed or unsafe input is never retried as a compatibility fallback.
6. The worker's chunk builder computes verified bounds, normals/tangents only where needed, and proxy/LOD and material references via meshoptimizer, then writes each chunk descriptor plus bytes into a bounded shared section and signals the control channel.
7. The loader/broker pool receives each descriptor, copies it to private host memory, bounds-checks every field against the section length and the generation's budgets, and copies the referenced bytes into host-owned memory or directly into the upload ring — only then is a chunk treated as validated. The shared section is not read again afterward.
8. Upload coordinator stages the smallest useful validated proxy and low-resolution or precomputed texture mips first, then visible/high-value detail.
9. Fence-complete chunks are published to render; render swaps scene/draw snapshots only at a frame boundary.
10. Residency manager continuously admits/evicts fine chunks against current DXGI budget. Proxy resources stay high priority/resident.
11. Once verified, the cache coordinator atomically commits a GPU-independent coarse proxy/catalog and eligible reusable chunks — built only from host-validated bytes — without delaying Ready. Replacement/cancel makes old-generation work drop results; render retires its scene only after a replacement proxy is ready unless memory pressure requires early release.

## Failure containment

| Failure | Required behavior |
| --- | --- |
| read/sidecar-open/cache failure in the trusted process | cancel that generation; UI/render continue; show stable error |
| stale/corrupt/full derived cache | treat as miss, quarantine/delete invalid entry when safe, continue uncached without UI/render wait |
| import-worker or compatibility-host crash/timeout/protocol fault/malformed section | terminate that process's job, discard its shared sections without treating any of their contents as validated, fail only that generation; a later open may create a fresh worker/host instance |
| worker allocation limit | lower optional detail where valid or return resource error |
| upload-ring full | upload thread backpressures; no UI/render wait |
| default-heap allocation/budget drop | evict fine LODs, reduce texture mips, retain proxy/UI |
| copy failure/device removal | stop publication, collect DRED, request render-owned device recovery |
| direct device removal | render falls back to UI error surface, recreates once, requests reupload from chunk store |
| later activation | cancel prior generation; stale messages/resources are reclaimed by generation/fences |
| thumbnail parser failure | HRESULT failure inside Shell isolation; no viewer impact |

## Intended repository layout

```text
Directory.Build.props/targets       shared compiler/link/security policy
Preview3D.slnx                      root solution
vcpkg.json                          dependency manifest
vcpkg-configuration.json            pinned registry baseline
.docs/design/                       normative documents
shared/model-core/
  include/                          normalized scene/chunk schema, wire-format contracts
  src/                              format adapters, resolver, normalization (linked only by import-worker/import-host)
  ModelCore.vcxproj                 static library
shared/import-broker/                broker protocol, AppContainer/Job Object launch, shared-section validation (shared by both import processes' host-side code)
shared/platform/                     Win32 RAII, checked math, paths, mapping
import-worker/
  src/                              general-format adapters (fastgltf/STL/PLY/ufbx/lib3mf/TinyUSDZ/Draco/KTX-Basis/WebP/DirectXTex-WIC/meshoptimizer), AppContainer entry point
  Preview3DImportWorker.vcxproj
compatibility-host/
  src/                              AppContainer OpenUSD adapter, entry point
  Preview3DImportHost.vcxproj
interactive-viewer/
  src/app/                          entry point, state, IPC, commands
  src/platform/                     Win32 window, mapping, RAII, accessibility
  src/graphics/                     D3D12 device/queues/ring/render/residency
  src/streaming/                    loader/broker pool, chunk validation/copy, cache, LOD/residency requests
  src/ui/                           chrome/overlays/text
  shaders/                          HLSL sources
  Preview3D.vcxproj
thumbnail-provider/
  src/                              COM provider and CPU rasterizer
  Preview3DThumbnailProvider.vcxproj
installer/                          WiX project/registration
tests/
  unit/                             core/platform/graphics/COM unit tests
  integration/                      viewer, IPC, GPU, Shell-host scenarios
  corpus/                           manifests, small licensed fixtures, regressions
  performance/                      large-fixture generators and benchmark harness
  fuzz/                             per-boundary fuzz targets
  import-isolation/                 broker, shared-section, restriction, and crash tests for both Preview3DImportWorker.exe and Preview3DImportHost.exe
third_party/notices/                pinned dependency notices
scripts/                            reproducible build/test/package commands
```

The current `interactive-viewer` code is a GLB-only vertical slice used to validate the responsive-shell/camera architecture; it does not yet have the import-worker/broker split above and parses GLB in-process on a background thread (see [ADR-014](./11-decisions-and-risks.md#adr-014-appcontainer-import-processes-are-the-parser-security-boundary-not-threads) for why that must not become the shipped ingestion path). Implementation removes global mutable application variables and splits responsibilities before broad-format feature work. Only x64 configurations ship; Win32 project configurations are removed or explicitly non-buildable to prevent accidental packaging.

`import-worker/Preview3DImportWorker.vcxproj`, `compatibility-host/Preview3DImportHost.vcxproj`, and `shared/model-core/ModelCore.vcxproj` currently exist only as empty scaffold projects (a buildable entry point with no parser/broker/AppContainer logic yet) so the solution structure is in place before Gate 2 wires in the real import sandbox; `shared/import-broker/` and `shared/platform/` are plain source directories consumed by the projects above rather than standalone projects.

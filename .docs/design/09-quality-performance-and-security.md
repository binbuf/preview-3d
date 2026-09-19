# Quality, performance, and security

## Release quality bar

The MVP is releasable only when responsiveness, bounded-resource behavior, malformed-input safety, visual correctness, installation lifecycle, and Explorer isolation have repeatable evidence. “It opens our sample files” is not sufficient.

All numeric results are reported with build ID, fixture hash, Windows/driver version, hardware, power mode, display refresh, cold/warm cache state, run count, median, p95, maximum, and failure count.

## Reference systems

### Performance reference

- current supported Windows 11 x64;
- 6-core-or-better desktop CPU comparable to Ryzen 5 5600;
- 32 GiB RAM;
- PCIe NVMe SSD with at least 3 GB/s sequential read;
- discrete D3D12 GPU comparable to GeForce RTX 3060 with 12 GiB local memory;
- 2560 × 1440, 144 Hz, plugged-in/high-performance power mode;
- current WHQL driver and no unrelated foreground workload.

This system owns the 144 Hz, startup, and multi-gigabyte timing gates. Results are not generalized to slower storage or every GPU.

### Compatibility reference

- supported Windows 11 laptop;
- 4-core-or-better CPU, 16 GiB RAM, NVMe;
- integrated D3D12 feature-level-11_0 GPU with shared memory;
- 1920 × 1080 at 60 Hz.

This system owns functional, memory-pressure, DPI, p95 loading-frame interval ≤16.7 ms, p95 input-to-affected-present ≤33 ms, 60 fps small/medium ready scenes, and 30 fps large-proxy behavior, but not the 144 Hz gate.

A WARP virtual machine runs deterministic correctness smoke tests only.

## Performance corpus

Release fixtures are immutable, checksummed, and distributable internally:

| ID | Format/content | Purpose |
| --- | --- | --- |
| A-small | 8 MiB GLB, 100k triangles, 4 materials | cold launch and first-model baseline |
| A-medium | 350 MiB GLB, 5m triangles, 2k nodes, textures | normal interactive target |
| A-large-glb | approximately 4 GiB GLB, 60m triangles, clustered spatially | mapping, proxy, out-of-core refinement |
| A-large-stl | approximately 3 GiB binary STL, 60m facets | sequential scanner/chunker |
| A-large-ply | approximately 3 GiB binary PLY, 60m points plus mesh variants | sequential scanner, stratified proxy, point/mesh rendering |
| A-adversarial-layout | 2 GiB glTF/BIN with sparse/nonlocal ranges | mapping-window and request-thrash behavior |
| A-compressed | GLB variants with Draco primitives, meshopt data, WebP, and KTX2/Basis textures | decode/transcode limits, low-mip-first behavior, compressed expansion |
| B-each | bounded representative OBJ/MTL, deformed FBX, extended 3MF, USDA, USDC, USDZ, ASCII STL/PLY | format fidelity and Tier B limits |
| USD-compat | composed local USD stages with sublayers, references, authored variants, instances, and payloads | TinyUSDZ fallback decision, broker resolver, OpenUSD host limits |
| Warm-cache | verified small/medium/large entries plus stale, corrupt, and version-mismatch variants | repeat-open latency, validation, fallback, eviction |
| Draw-heavy | many nodes/instances/material groups with modest triangle count | batching and direct-versus-indirect submission threshold |
| Pressure | medium scene plus synthetic DXGI budget reduction | eviction and proxy survival |
| Malformed | truncated, overflow, invalid graph, archive bomb, hostile paths | safety and recovery |

Content generation recipes and expected counts/bounds accompany generated large files so CI need not store every multi-gigabyte binary in Git.

## Performance gates

The formal gates restate and make measurable the NFRs in [01-product-scope.md](./01-product-scope.md):

| Measure | Gate |
| --- | --- |
| Cold empty launch to first background present | p95 ≤150 ms on performance reference |
| File launch to loading UI present | p95 ≤200 ms |
| Loading/empty frame interval at 144 Hz | p95 ≤8.3 ms; no product-load-caused interval >50 ms |
| Pointer event to affected present, performance reference | p95 ≤16 ms during large import |
| Loading frame / input latency, compatibility reference | p95 frame interval ≤16.7 ms and p95 input-to-affected-present ≤33 ms |
| A-small first complete proxy | ≤500 ms |
| A-medium first complete proxy | ≤2 s |
| A-large Tier A first useful partial proxy | ≤2 s and complete coarse proxy ≤5 s |
| Verified warm-cache A-small / A-medium complete proxy | p95 ≤250 ms / ≤750 ms |
| Verified warm-cache A-large complete coarse proxy | p95 ≤2 s |
| Normal cancellation observation | p95 ≤100 ms; maximum 500 ms outside documented library calls |
| Import-worker or compatibility-host cancellation | acknowledgement ≤500 ms or bounded Job Object termination without viewer stall |
| UI message heartbeat during any load | no gap >100 ms attributable to product work |
| A-large private committed CPU memory | ≤1.5 GiB beyond baseline; mapped-file views reported separately |
| Import-worker Job Object private commit | measured and fixed in Gate 2 to comfortably exceed the worst-case sum of the Tier A/B scratch, texture, and archive-expansion budgets in [03-file-formats-and-ingestion.md](./03-file-formats-and-ingestion.md); controlled failure at cap |
| OpenUSD compatibility-host private commit | within the lower of 4 GiB and 35% of physical RAM; controlled failure at cap |
| Persistent derived cache | ≤10 GiB default soft cap, ≤2 GiB per entry, and no admission below free-space floor |
| Upload staging | ≤512 MiB and never overlaps an unretired ring range |
| GPU model allocation | within current product cap; budget reduction sheds detail without allocation storm |
| Thumbnail p95 / cutoff | ≤750 ms p95 on representative files; internal cutoff 2 s |

Warm-cache latency gates use local fixtures whose open handles expose the stable volume/file and change-journal version tokens defined in [04-rendering-and-streaming.md](./04-rendering-and-streaming.md). Results state the identity-validation mode. Full-content-digest fallback remains a correctness path, not a promise that an unchanged multi-gigabyte file on storage without reliable version evidence will meet the warm gate.

“First useful partial proxy” must contain at least the spatially representative sample threshold defined by the fixture, not merely one arbitrary triangle or point. “Complete coarse proxy” covers every nonempty model component above the sampler's minimum projected/volume threshold and has verified scene bounds.

Frame intervals are classified using ETW present events. Occlusion, monitor mode change, debugger break, OS scheduling interruption, shader-cache/driver installation, and display-vsync multiples are retained in raw data but separately labeled; exclusions require an automated reason, not manual deletion.

For Tier B, publish per-format and per-importer-path median/p95 import throughput and peak memory. Release gates are the hard limits, responsiveness, no crash/hang, and the medium-file ≤2 s goal where the representative fixture is within the medium workload—not a multi-gigabyte promise. Cache-hit measurements include lookup and validation and are reported separately from OS file-cache warmth.

## Test layers

### Unit and property tests

- checked offset/count/pitch math and allocation budgets;
- mapping-window alignment, leases, EOF spans, file identity changes;
- every format adapter's token/range/graph validation;
- PLY schema/list/endianness/property skipping and point/mesh normalization;
- derived-cache keying, schema migration rejection, checksums, atomic commit, free-space policy, LRU, and clear races;
- import protocol framing, shared-section range/checksum validation, broker path decisions, and Job Object policy, exercised identically against `Preview3DImportWorker.exe` and `Preview3DImportHost.exe`;
- transform, up-axis/unit conversion, camera-relative precision, AABB/sphere;
- triangulation, normal/tangent generation, cluster splitting, LOD error;
- upload ring allocate/wrap/backpressure/retire model with generated fence sequences;
- immutable snapshot generation and deferred-release fence calculation;
- path canonicalization and sidecar/archive-entry policy;
- pipe frame/UTF-8/JSON/parser/security checks;
- CPU rasterizer clipping, depth, alpha, premultiplication;
- COM identity/refcount/lifetime and HRESULT mapping.

Property tests generate counts and ranges near 0, alignment boundaries, 32-bit/64-bit limits, and maximum values. They assert rejection happens before allocation or pointer arithmetic.

### Integration tests

- real mapped files through normalized chunks and a headless validation sink;
- TinyUSDZ fast-path and AppContainer OpenUSD-host overlap scenes producing equivalent normalized results;
- warm-cache hit, dependency change, corrupt entry, interrupted write, cap/free-space eviction, disable, and clear scenarios;
- upload coordinator against WARP and hardware with copy/direct fence permutations;
- renderer golden scenes and camera controls at multiple aspect ratios/DPI;
- device removal injection and one-shot recovery;
- cancellation/reopen at every published pipeline checkpoint;
- Shell provider hosted in an isolated COM test process and actual Explorer surrogate, with a post-install check that its CLSIDs load into the Shell thumbnail surrogate rather than `explorer.exe` and that no registered value sets `DisableProcessIsolation`;
- primary/secondary IPC under concurrent launches;
- zero-capability AppContainer creation, payload-only ACLs, brokered handle/section access, direct file/network denial, Job Object termination, and restart, run against **every** import process — `Preview3DImportWorker.exe`, `Preview3DImportHost.exe`, and `Preview3DStepHost.exe` — every format adapter is covered by the same restriction suite the OpenUSD host already required, not a lighter check because it "only" runs a fast-path parser;
- a synthetic hostile-worker build for each import process that mutates shared-section bytes after the host's first read, replays a stale generation, or lies about a chunk's declared layout/offset, proving the host's copy-then-validate rule actually rejects the mutation rather than merely trusting a well-behaved worker;
- MSI clean/repair/upgrade/rollback/uninstall virtual-machine matrix.

### End-to-end soak

An 8-hour scenario repeatedly opens mixed valid/malformed files through cold/cache/worker/compatibility/STEP paths, orbits, resizes, minimizes, changes DPI/monitor, cancels, clears/rebuilds the cache, crashes/restarts the import worker, the compatibility host, and the STEP host, lowers memory budget, and closes/reopens. It fails on:

- process/worker/surrogate crash or hang;
- D3D debug-layer error/corruption warning;
- GDI/User/handle/thread count growth beyond established noise;
- monotonic private-commit or descriptor/resource growth;
- stale generation becoming visible;
- a stale/corrupt cache entry or invalid worker/host section becoming visible;
- an import worker, compatibility host, STEP host, temporary cache write, or broker handle surviving its generation;
- UI heartbeat or shutdown deadline violation.

## Graphics validation

Developer validation builds enable:

- D3D12 debug layer and synchronized command-queue validation;
- GPU-based validation in dedicated, slower suites;
- DRED auto-breadcrumbs and page-fault reporting;
- DXGI debug live-object reporting at controlled shutdown;
- PIX/ETW markers for every frame, copy batch, generation, and resource lifetime;
- shader debug names and stable pipeline hashes.

No release build enables expensive validation by default. A signed diagnostics mode can enable redacted ETW/file logs without changing synchronization.

GPU tests cover integrated/discrete adapters, resize/full occlusion, display sleep/wake, adapter removal simulation where available, shared-memory budget changes, low local budget, allocation failure, delayed fences, and copy queues that do not physically overlap graphics. Correctness cannot depend on queue overlap.

## CPU and concurrency validation

Debug/CI configurations include iterator/runtime checks where compatible, AddressSanitizer x64 for non-COM/headless and application scenarios, static analysis, compiler warnings as errors for product code, and Application Verifier/PageHeap runs. Clang-cl sanitizer/fuzzer builds may coexist with the supported MSVC release build.

Deterministic scheduler hooks pause tasks immediately before/after:

- mapping lease release;
- generation cancellation;
- queue enqueue/dequeue;
- copy submission/fence observation;
- snapshot publication;
- LOD eviction/direct-fence retirement;
- resize/device loss/shutdown.

Tests enumerate adversarial interleavings. Thread termination, SuspendThread-based coordination, unbounded condition-variable waits, and sleeping for correctness are prohibited.

## Fuzzing

Each format has a standalone, no-GPU fuzz target that accepts bytes plus a constrained virtual sidecar/archive map and runs through normalized metadata/chunk output. Additional targets cover:

- GLB/glTF JSON/accessor/range validation;
- binary/ASCII STL detection/tokenization;
- binary/ASCII PLY header, endianness, scalar/list, point, and polygon normalization;
- Draco bitstreams, KTX2/Basis level metadata/transcode boundary, WebP, and expanded texture metadata;
- OBJ/MTL and FBX adapter options/callbacks;
- 3MF/USDZ archive directory and expansion accounting;
- USDA/USDC object graphs;
- import-worker and compatibility-host protocol/shared-section descriptors and broker dependency requests, including the wire-format header/chunk-descriptor decoder itself;
- persistent-cache manifests, indexes, section tables, and normalized payloads;
- image metadata/decode boundary;
- IPC frame and JSON;
- CPU thumbnail clipping/raster setup.

Seed corpora include official conformance assets, product regressions, minimized crashes, unsupported feature examples, and cross-format mutations. Continuous fuzzing uses ASan/UBSan-compatible builds; every unique crash, timeout, excessive allocation, or assertion is minimized and becomes a regression test. Parser dependency updates must run the complete corpus before merge.

## Threat model

An attacker may control:

- primary model bytes, names, declared counts, compression, graphs, and embedded data;
- local sibling names/content reachable from a user-selected folder;
- per-user derived-cache files and indexes modified or replaced by the current user or local malware;
- Shell IStream behavior;
- IPC connection attempts and payload bytes from local processes;
- import-worker and compatibility-host exit behavior, control frames, dependency requests, and shared-section bytes — including a fully compromised worker/host that deliberately mutates a shared section after the host's first read, since Windows gives every mapped view of a section coherent live access for as long as it stays mapped;
- extreme GPU workload intended to trigger timeout/removal;
- archive nesting/paths and Unicode edge cases.

The attacker must not be able to execute code, escape the model directory, access network resources, disclose unrelated files through rendering/logs, exhaust resources without a bound, establish executable/automatic persistence through the app, or destabilize Explorer beyond one failed thumbnail request.

Windows, the signed installed payload, and the graphics driver are trust dependencies, though driver failure is handled. Models and all parser output are untrusted.

## Security controls

### Process and binary

- DEP/NX, ASLR/high entropy VA, CFG, CET compatibility, SDL checks, and stack protection enabled.
- Safe DLL search established before optional loads; current directory and model directory never enter DLL search.
- The viewer loads no third-party format parser or decoder and no runtime plug-ins, scripts, shader compiler, environment-selected codecs, or product network stack; every such library loads only inside `Preview3DImportWorker.exe` (general formats), `Preview3DImportHost.exe` (OpenUSD), or `Preview3DStepHost.exe` (OCCT/STEP). The thumbnail provider loads its own bounded copies under Shell's process isolation, per [05-thumbnail-provider.md](./05-thumbnail-provider.md). Every import process loads only release-manifest-listed, signed app-local modules and hash-verified resources by absolute path after DLL search and plug-in discovery are locked down.
- Release loads only system components through documented mechanisms and product binaries by absolute installed path.
- Authenticode and dependency/SBOM controls follow [08-installation-and-registration.md](./08-installation-and-registration.md).

### Files and archives

- Handle-based canonical path checks and FILE_SHARE_READ-only lifetime policy reduce time-of-check/time-of-use changes.
- All sizes/counts use checked 64-bit math before conversion to size_t/D3D types.
- Allocation, depth, object, dependency, time, decoded-pixel, and archive-ratio budgets are enforced through mandatory parser callbacks inside the owning import process and, independently, by that process's broker/Job Object limits — a callback bug in one layer does not remove the other.
- Archive entry names are canonicalized as virtual relative paths; absolute, drive, device, alternate-stream, traversal, duplicate-conflicting, and symlink-like entries are rejected.
- Temporary normalized stores use random names, current-user-only ACLs, delete-on-close handles, and non-executable data.
- Persistent cache directories use current-user-only ACLs. Entries use opaque names, bounded manifests, checked section tables, and cryptographic checksums; data is revalidated as untrusted normalized input before upload.
- Mapping/view access is contained by validated spans; product code does not dereference file-derived pointers outside a live lease.

### Rendering

- Files cannot supply shader bytecode, root signatures, command data, executable code, or arbitrary GPU virtual addresses.
- GPU buffer sizes/offsets/strides and Draw arguments are generated from revalidated normalized descriptors.
- Non-finite/out-of-range transforms and vertices are rejected or safely clamped before float conversion.
- Allocation failure, device removal, and budget loss degrade to proxy/error rather than repeated retry.

### Shell and IPC

- The thumbnail provider observes the stricter policy in [05-thumbnail-provider.md](./05-thumbnail-provider.md), including no path/sidecar/network access, and depends on Shell's default surrogate-process isolation rather than any in-process mitigation; `DisableProcessIsolation` MUST NOT be set for its CLSIDs. That surrogate isolation is crash containment for Explorer, not the zero-capability AppContainer security boundary the import processes below provide — the provider's safety against a hostile file rests on its own bounded reads and checked parsing, not on the surrogate process.
- Named objects use explicit current-user/session ACLs; clients are authenticated and payloads bounded.
- The receiver never invokes a shell with model-derived text.
- `Preview3DImportWorker.exe`, `Preview3DImportHost.exe`, and `Preview3DStepHost.exe` are launched with a zero-capability AppContainer token and assigned to a kill-on-close Job Object *before* they process any input — the broker launches suspended or assigns the job at process-creation time so no import code ever runs under a less-restricted intermediate state. None can access the network/model directory directly or create child processes, and each receives model dependencies only through the parent broker via an explicit `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, not broad handle inheritance. The parent rejects unexpected request order, unknown message/section versions, stale generations, and invalid ranges/checksums, and — because a shared section stays writable by the child for as long as it is mapped — never treats a chunk as trusted until it has copied the chunk's descriptor and bytes into its own memory and independently re-validated them; the shared section itself is never re-read afterward.

### Privacy

There is no telemetry or crash upload in MVP. Default release logs contain timestamps, build/device identifiers, stage/code/counters, cache hit/miss reasons, import-worker and compatibility-host exit codes, and correlation IDs but not file content, material/mesh names, full paths, or rendered images. Copy details includes paths only after explicit user choice. Temporary data is deleted on close/crash cleanup at next launch. Persistent derived entries contain renderable model-derived geometry/textures, are bounded to the current user's profile, omit names/paths, can be disabled, and can be removed with Clear cached previews.

## Dependency policy

Every third-party component needs:

- pinned release/commit and verified source hash;
- compatible license/notice;
- active vulnerability review and upstream security watch;
- wrapper ownership of allocation, I/O, errors, and cancellation;
- fuzz/conformance evidence for enabled features;
- no unreviewed transitive dynamic dependency;
- an upgrade/rollback record.

Warnings and exceptions from dependency headers are contained at a dedicated build target boundary; product code remains warning-clean. Updating a parser is a behavior change requiring corpus, performance, memory, thumbnail-host, and installer license retest. Updating any import-worker or compatibility-host module — the boundary is symmetric between them — additionally requires broker/protocol, restriction, composed-stage/normalization, binary-size/startup, and signed-payload retesting. Updating Draco, KTX/Basis, libwebp, or DirectXTex requires compressed-expansion and image fuzz corpora plus cache-version review.

## CI and release evidence

Per change:

- x64 Debug and Release build;
- unit/property tests;
- lint/static analysis;
- headless adapter regression corpus;
- short ASan/fuzz smoke;
- package manifest validation.

Nightly:

- hardware D3D debug and performance smoke;
- malformed/cancellation/device-pressure matrix;
- persistent-cache, import-worker, and compatibility-host fault matrix;
- thumbnail surrogate stress;
- install lifecycle VMs;
- longer fuzzing.

Release candidate:

- full reference performance runs from clean boot/cache-defined conditions;
- 8-hour soak on performance and compatibility systems;
- all supported Windows 11 update baselines in the support matrix;
- dependency/license/SBOM/signature scan;
- clean D3D/DXGI/Application Verifier logs;
- manual keyboard, high contrast, 100/150/200% DPI review;
- signed MSI install/upgrade/uninstall evidence.

Every gate links raw traces and exact fixture hashes. A waiver names owner, user-visible effect, expiry, and mitigation; security crashes, Explorer hangs, data disclosure, stale-resource rendering, or UI-thread waits cannot be waived for MVP.

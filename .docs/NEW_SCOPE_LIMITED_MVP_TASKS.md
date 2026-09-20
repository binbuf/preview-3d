# Preview 3D: Scope-Limited MVP Task Manifest

> Post-MVP delivery amendment (2026-09-16): the product owner approved ASCII
> STL and ASCII PLY as the first two Tier B inputs. They ship through the same
> AppContainer/broker path and emit bounded progressive batches, but use a
> materializing parser with lower limits: 2 GiB primary source, 20 million
> triangles or points, 60 million expanded vertices, and scratch capped at the
> lower of 1.5 GiB or 35% of physical RAM. Tier A coarse-proxy/refinement and
> multi-gigabyte performance promises do not apply. All other Tier B formats
> remain excluded.
>
> Post-MVP delivery amendment (2026-09-16): the product owner requested an
> NSIS installer and supported-extension default-app integration after the
> portable MVP was completed. This supersedes only the portable-only/no-file-
> association delivery statements below. The installed payload remains the
> same viewer plus general import worker, registers only `.glb`, `.gltf`,
> `.stl`, and `.ply`, and opens the Windows 11 Default Apps confirmation UI;
> it does not overwrite a protected per-user default choice. Explorer
> thumbnails, COM handlers, other Tier B formats, compatibility host, persistent
> model-derived cache, services, and background processes remain excluded.
>
> Post-MVP amendment (2026-09-16): after the scope-limited implementation was
> frozen, the product owner explicitly approved one viewing extension: a
> persisted title-bar X/Y/Z model ground-axis cycle and signed-direction flip. It is implemented as an
> additive post-MVP feature and does not reopen the Tier B, Shell registration, model-derived cache, LOD,
> or compatibility-host exclusions below.
>
> Post-MVP amendment (2026-09-16): the product owner also approved a bounded
> enhancement to the existing title-bar Open With surface. It recognizes a
> curated list of popular CAD, modeling, and 3D-printing applications from
> Windows-registered handlers, persists only handler identity/display metadata,
> discovers additions asynchronously on a seven-day cooldown, and invalidates
> an entry after an invocation error. This does not add file associations,
> Default Apps registration, an installer, model-path history, model-derived
> data, executable command-line guessing, network access, or a new UI surface.

This document outlines the sequential LLM prompts required to reach a deployable MVP, originally restricted strictly to Tier A formats (glTF, STL, PLY) and the existing UI. Except for the explicit post-MVP amendments above, Tier B formats, the persistent cache, meshoptimizer clustering, and the thumbnail provider are out of scope for this pass.

## Phase 1: Renderer Consolidation & UI Hookup

### TSK-101: D3D12 Default Cutover

* **Recommended Model:** Claude Code (stronger at cross-file architectural changes)
* **Context Files to Load:** `interactive-viewer/src/app/Preview3D.cpp`, `interactive-viewer/src/app/D3D12ViewerPath.cpp`, `interactive-viewer/src/render/Renderer.h`
* **Prompt Instructions:**
1. Deprecate the `useD3D12` flag and the legacy D3D11 `Renderer` path in `Preview3D.cpp`.


2. Make `D3D12ViewerPath` the exclusive rendering path for the application.


3. Ensure `WM_CREATE`, `WM_SIZE`, and `WM_DESTROY` route unconditionally to their D3D12 equivalents.


4. **Constraint:** Do not delete `Renderer.cpp` yet, as its D2D overlay code is still needed for reference.


* **Verification:** `msbuild Preview3D.slnx /t:Preview3D /p:Configuration=Debug`

### TSK-102: D2D Chrome Port to D3D11On12 Overlay

* **Recommended Model:** Claude Code
* **Context Files to Load:** `interactive-viewer/src/graphics/D3D11On12Overlay.cpp`, `interactive-viewer/src/render/Renderer.cpp`, `interactive-viewer/src/app/D3D12ViewerPath.cpp`
* **Prompt Instructions:**
1. Migrate the ~750 lines of Direct2D chrome drawing logic out of `Renderer.cpp` into the newly built `ID2D1DeviceContext` overlay bridge inside `D3D11On12Overlay.cpp`.


2. Ensure the overlay draws the title bar, bottom bar, info panel, and navigation gizmo over the D3D12 swap chain.


3. Remove the `--overlay-spike` stand-in primitives.


4. **Constraint:** The overlay must utilize the single `overlayBrush` and per-back-buffer bitmaps established in the bridge update.




* **Verification:** `msbuild Preview3D.slnx /t:Tests_Unit`

### TSK-103: Restore UI State Binds

* **Recommended Model:** Codex (excellent at targeted, repetitive logic fixes)
* **Context Files to Load:** `interactive-viewer/src/app/Preview3D.cpp`
* **Prompt Instructions:**
1. Locate the six call sites checking `app.renderer.HasModel()` (`HasNavigableModel`, `FrameSelectedOrAll`, `ToggleShowNativeOrientation`, `CancelOpen`, `ID_VIEW_RESET`, and the bottom bar).


2. Update these checks to evaluate the `d3d12Path.hasModel` state instead, so UI elements no longer silently no-op.


3. **Constraint:** Do not alter the camera math or viewport aspect ratio logic.


* **Verification:** Run `Preview3D.exe` and confirm the bottom bar and info panel reserve space when a model is loaded.

---

## Scope contract for the remaining work (reviewed 2026-09-15)

TSK-101, TSK-102, and TSK-103 are complete per the current handoff. Their prompts above are retained unchanged as history; do not repeat them. Start with TSK-104, then execute the remaining tasks in numeric order. Each task below is remaining work, even where a tested primitive already exists.

This is a **viewer-only, scope-limited MVP**, not completion of every requirement in the original [product scope](./design/01-product-scope.md) or every gate in the [delivery plan](./design/10-delivery-plan.md). The following table clarifies the introductory shorthand without changing completed Phase 1 work.

| Area | Commitment for this pass |
| --- | --- |
| Environment | Serviced Windows 11, x64, D3D12 feature level 11_0 or later; local regular files and Unicode paths. WARP is a correctness/diagnostic fallback. |
| Direct inputs | `.glb`, `.gltf` with broker-approved local binary/image sidecars, ASCII/binary STL, and supported ASCII/binary little/big-endian PLY meshes **and point clouds**, case-insensitively. ASCII STL/PLY are post-MVP Tier B additions with lower limits and no Tier A performance promise. |
| Rendering | Static meshes/instances, source or generated normals, vertex colors, studio lighting, and glTF metallic/roughness, unlit, alpha, double-sided, and texture-transform semantics. PNG/JPEG and existing KTX2/Basis support remain; add WebP as part of the documented glTF subset. |
| Large models | Representative early geometry, verified bounds, a bounded complete coarse proxy, and view-prioritized fine detail under the live DXGI budget. No whole-source private heap copy or whole-scene normalized payload retained in either process. |
| Existing UI | Preserve chrome, layout, camera feel, fullscreen, gizmo, Info, Fit/Reset, native orientation, and existing selection behavior. Update format text, real metadata, loading/error/warning states, and accessibility within these surfaces. |
| Delivery | Portable viewer ZIP plus an additive per-machine NSIS installer. The installer registers Default Apps/Open With capabilities only for the four direct extensions and preserves Windows user choice. No COM registration, thumbnail provider, MSI lifecycle, or other Explorer integration is claimed. |
| Deferred | Tier B formats, thumbnails, compatibility host, persistent derived cache and warm-cache gates, meshoptimizer **LOD/cluster construction**, intermediate LODs/cross-fades, TGA/DDS/HDR, and indirect submission optimization. Bounded chunk splitting and coarse sampling are still required; meshopt **compressed-data decoding** is included. |

The original responsiveness, parser isolation, handle-based path policy, copy-then-validate acceptance, generation filtering, bounded queues/allocations, and fence-safe publication/retirement requirements still apply. All parsing, normalization, image decode, and compressed-data decode stay inside the zero-capability AppContainer worker. Disk/broker waits, normalization, bounds scans, default-heap allocation, and copy recording/submission stay off the UI thread; upload allocation/submission must also leave the presenting render thread. No product network access or persistent model-derived writes are introduced.

Use [07-user-experience.md](./design/07-user-experience.md) for the existing visual identity, with the current code as the implemented reference. Its GLB-only strings and historical implementation notes must be updated for this pass; they are not reasons to preserve inaccurate format/error/statistics text. Scope exclusions above override broader feature breadth, not security or responsiveness invariants. Record any further invariant change as an ADR with revised acceptance tests, per the delivery plan's change-control rule.

**Verification convention:** solution targets build binaries; they do not run Catch2. For every affected slice, build through `Preview3D.slnx` so app-local dependencies are deployed, then execute the affected `x64/<Configuration>/Tests.Unit.exe` and/or `Tests.ImportIsolation.exe`. Run relevant suites in Debug and Release before release qualification. A build alone, or a KB-sized triangle smoke test alone, does not establish MVP readiness.

### TSK-104: Fixture Manifest and Repeatable App Smoke Tests

* **Recommended Model:** Codex
* **Context Files to Load:** `tests/unit/`, `tests/import-isolation/`, `interactive-viewer/test-assets/`, `interactive-viewer/src/app/Preview3D.cpp`, `design/09-quality-performance-and-security.md` (under `.docs/`)
* **Prompt Instructions:**
1. Add checksummed fixture manifests with expected counts, bounds, material/color/texture results, and representative-proxy thresholds. Provide deterministic generation recipes for A-small, A-medium, A-large GLB/STL/PLY (mesh and points), adversarial glTF sidecars, compressed glTF, Draw-heavy, Pressure, and malformed/over-limit cases. Do not commit multi-gigabyte binaries.
2. Add repeatable app smoke automation for open/replace/cancel/resize/close and first-background/first-geometry milestones. Use existing developer/test seams where suitable; do not require manual dialogs to exercise a second open.
3. Keep fast small fixtures in routine checks and large generated fixtures in an explicit qualification lane. Establish baseline timing/memory evidence now; the complete performance harness is TSK-302.
* **Verification:** Generate small fixtures twice and compare hashes/expected metadata; run both Catch2 binaries and a real-app open/replace/close smoke. Document exact commands and fixture generation requirements.

---

## Phase 2: Tier A Progressive Pipeline

### TSK-201: Bounded Progressive Display Wiring

* **Recommended Model:** Claude Code
* **Context Files to Load:** `interactive-viewer/src/app/D3D12ImportBridge.cpp`, `interactive-viewer/src/app/RenderThread.cpp`, `interactive-viewer/src/app/D3D12ViewerPath.cpp`, `shared/import-broker/src/ImportSession.cpp`, `import-worker/src/ChunkBatchSink.cpp`
* **Prompt Instructions:**
1. Supply `ImportSessionRequest::onBatch` and move host-owned, copied-and-validated batches into a byte/count-bounded, cancellation-aware upload queue. Remove terminal whole-import accumulation in streaming mode; the batch callback must not retain shared-section pointers.
2. Put default-heap allocation and copy recording/submission on an upload coordinator. The render thread only consumes fence-complete publications and keeps presenting the prior scene/chrome while a copy is delayed or the upload ring is full.
3. Publish geometry additively by generation/chunk identity. Define and validate generation-wide material/image dependencies across batches (the existing per-section checks alone are insufficient), with neutral fallbacks and immutable frame-boundary updates. Bound any unresolved references and reject invalid terminal catalogs. Never overwrite an earlier chunk or declare Ready solely because one batch was copied.
4. Send `ChunkBatchConsumed` only after the host owns the accepted batch and bounded downstream capacity permits progress. Cancel/close must unblock backpressure and discard stale work. Retire superseded GPU resources behind both relevant fences.
5. Preserve the old model until the replacement has a usable representation. Early partial content must remain labeled Loading; TSK-206 establishes the complete-proxy handoff for large scenes.
* **Verification:** Execute import-isolation batch/hostile-worker tests and an app test with a delayed final batch. First geometry must present before terminal IPC, the old model and input must remain responsive under a forced copy delay, and queue byte caps must hold during replace/cancel.

### TSK-202: Verified Bounds, Cluster Origins, and UI Metadata

* **Recommended Model:** Codex
* **Context Files to Load:** `shared/model-core/include/model_core/WireFormat.h`, `shared/import-broker/src/SharedSectionValidator.cpp`, `import-worker/src/GltfAdapter.cpp`, `import-worker/src/StlAdapter.cpp`, `import-worker/src/PlyAdapter.cpp`, `interactive-viewer/src/app/D3D12ViewerPath.cpp`, `interactive-viewer/src/ui/InfoPanel.cpp`, `interactive-viewer/src/app/Preview3D.cpp`
* **Prompt Instructions:**
1. Add fixed-width double-precision cluster origins, local bounds, and generation-tagged scene metadata sufficient for format, units/up axis, counts, and provisional/verified bounds. Bump the protocol version for wire-layout changes; update producers, broker checks, dispatch, static assertions, fixtures, and fuzz/hostile cases together. Reject unknown versions.
2. Normalize positions relative to cluster origins **inside the worker**. Reduce verified bounds incrementally from finite geometry; accessor min/max are provisional. The host validates copied origins/bounds and their geometric consistency before using them for culling or camera placement.
3. Render camera-relative float transforms derived from double origins/transforms. Carry mesh/node/material identities and counts without duplicating all vertex payloads for the UI. Cover triangles and points; do not skip `PositionOnly_F32`.
4. Publish immutable metadata to the UI for accurate Info statistics/dimensions, Fit/Reset framing, and native-orientation transforms. Preserve existing camera math/layout; apply provisional-to-verified framing corrections according to the user's interaction epoch.
5. Keep existing selection/Frame selected functional through bounded pick data or source-range queries. Do not rebuild a whole-scene CPU mesh solely for picking, and do not expand selection features.
* **Verification:** Execute unit precision/bounds/wire tests and import-isolation tests for NaN/Inf, fabricated bounds, malformed origins, stale metadata, and protocol mismatch. In the app, verify large-offset/tiny-scale meshes and point clouds, real Info counts, native orientation, and camera corrections after user movement.

### TSK-203: Complete the Existing Texture Path

* **Recommended Model:** Codex
* **Context Files to Load:** `import-worker/src/WicImageDecodeAdapter.cpp`, `import-worker/src/TextureTranscodeAdapter.cpp`, `import-worker/src/GltfAdapter.cpp`, `shared/model-core/include/model_core/PixelFormats.h`, `shared/import-broker/src/SharedSectionValidator.cpp`, `interactive-viewer/src/app/D3D12ViewerPath.cpp`
* **Prompt Instructions:**
1. Preserve and verify the already-working WIC PNG/JPEG decode and `ImagePayloadHeader+pixelBytes` emission; do not schedule their implementation from scratch. Preserve existing BMP/TIFF paths without expanding the direct glTF image allowlist.
2. Add aggregate encoded/decoded byte and pixel budgets, safe decoder selection restricted to enabled inbox codecs, and cancellation checkpoints. Keep every decoder/transcoder in the worker.
3. Generate ordinary raster mips in bounded cancellable tiles, use trustworthy decoder downscaling when available, and validate/upload existing KTX2/Basis mip levels. Publish small mips before larger immutable replacements; cap texture resolution under budget pressure.
4. Honor sRGB color versus linear data/normal semantics and semantic transcode targets with validated RGBA fallback. Missing/corrupt/unsupported optional textures produce deterministic checker/neutral fallbacks and a bounded warning, without losing valid geometry.
5. Keep TGA/DDS/HDR deferred; WebP is added in TSK-209. Material shader consumption is TSK-208.
* **Verification:** Execute texture decode/transcode/validator suites, including hostile dimensions, row pitches, mip sizes, and aggregate expansion. Use pixel readback/golden scenes for PNG/JPEG and KTX2/Basis, color space, fallback, low-mip-first publication, and cancellation.

### TSK-204: Recoverable Errors, Warnings, and Loading State

* **Recommended Model:** Codex
* **Context Files to Load:** `interactive-viewer/src/app/D3D12ImportBridge.cpp`, `interactive-viewer/src/app/Preview3D.cpp`, `interactive-viewer/src/graphics/D3D11On12Overlay.cpp`, `shared/model-core/include/model_core/ImportError.h`, `shared/import-broker/src/ImportSession.cpp`
* **Prompt Instructions:**
1. Reuse the error descriptions already in `D3D12ImportBridge`; complete typed code/stage propagation for this pass's failures, including unsupported required features/encoding, empty geometry, out-of-memory, source changes, worker crash/timeout/limit, and protocol violations. Cancellation is not a document error.
2. Wire Retry, Open another, and Copy details to the existing card. Report actual source format and failing phase, never hardcoded GLB/opening. Omit source paths from copied/default diagnostics.
3. Add a bounded, validated warning/status payload for optional feature/texture fallback and provisional/refining/pressure state. Reuse the existing warnings menu and spinner/card surfaces; no new telemetry service or UI redesign.
4. Update picker filters, drop-target/About/help text, remote-path errors, and unsupported-format guidance to the scope table. Distinguish a recognized extension with a deferred ASCII encoding from malformed data.
5. Keep prior content interactive on failure and accept a later valid activation without relaunch. Mark Ready only after verified bounds and the complete coarse catalog exist, not after the first progressive batch.
* **Verification:** Automate unsupported FBX, ASCII policy, malformed/empty Tier A files, unsafe/missing sidecars, worker crash/limit/timeout, upload failure, and retry/valid reopen. Verify typed messages, warning truncation limits, actual format/phase in Copy details, and stale-generation rejection.

### TSK-205: Bounded Tier A Scanning and Chunk Splitting

* **Recommended Model:** Claude Code
* **Context Files to Load:** `import-worker/src/GltfAdapter.cpp`, `import-worker/src/StlAdapter.cpp`, `import-worker/src/PlyAdapter.cpp`, `import-worker/src/ChunkBatchSink.cpp`, `shared/model-core/src/MappedFile.cpp`, `shared/import-broker/src/ImportSession.cpp`
* **Prompt Instructions:**
1. Replace whole-model normalization and single-chunk STL/PLY output with mapped-range parse/normalize/emit/free loops. Split oversized glTF primitives and binary STL/PLY into roughly 4–16 MiB chunks, at most 262,144 triangles or 1,048,576 points, preserving material/node/instance identity and source-range provenance.
2. Bound remapping, polygon triangulation, sparse accessor handling, deduplication, and index lookup to clusters/windows. For PLY faces referencing nonlocal vertices, use bounded indexed mapped access; never require all source positions in private memory. Support both binary endiannesses and bounded skipping of unknown properties/lists.
3. Enforce the applicable Tier A limits from `.docs/design/03-file-formats-and-ingestion.md` before allocation, including scratch <= min(1 GiB, 25% physical RAM), source/sidecar/count/texture caps, and independently bounded Draco primitives. Job termination is a backstop, not the routine budget check.
4. Derive batch/catalog ceilings from worst-case normalized expansion and split sizes, not the assumption that normalized output is always smaller than source. Backpressure must keep accepted CPU payloads bounded in both processes.
5. Retain a bounded source-range catalog for later fine-detail re-decode through the worker. Do not introduce a persistent cache or retain every normalized chunk; malformed ranges, changed sources, and deferred layouts fail safely with a named limit/encoding.
* **Verification:** Generate and load 2–4 GiB GLB, binary STL, and binary PLY mesh/point fixtures plus oversized single primitives and adversarial nonlocal faces/accessors. Assert first batches arrive before full normalization, steady bounded scratch/queues, correct counts/bounds, cooperative checkpoints, and no source-sized private heap allocations.

### TSK-206: Representative Coarse Proxy Without a Full LOD Builder

* **Recommended Model:** Claude Code
* **Context Files to Load:** `import-worker/src/GltfAdapter.cpp`, `import-worker/src/StlAdapter.cpp`, `import-worker/src/PlyAdapter.cpp`, `interactive-viewer/src/app/D3D12ViewerPath.cpp`, `.docs/design/03-file-formats-and-ingestion.md`
* **Prompt Instructions:**
1. Produce deterministic, validated samples from stratified spatial/source ranges so first geometry represents the fixture, rather than merely its first triangle. Include nonempty components and preserve recognizable boundaries/material groups; point clouds use bounded spatial/reservoir sampling.
2. Build a complete coarse catalog capped by the lesser of 2 million primitives, the reserved GPU budget, and the density target in [ADR-015](./design/11-decisions-and-risks.md#adr-015-bounded-coarsefull-sampling-and-the-mandatory-coverage-floor): 5% of valid source primitives, raised only to one primitive per nonempty source region when necessary for complete coverage; valid documents below 20 primitives retain all primitives. Verified scene bounds still require the complete scan. This explicit coverage-floor exception resolves the otherwise impossible cap for separate single-triangle instances such as Draw-heavy.
3. Give coarse regions and their fine replacements stable relationships. Keep the proxy always available, remove a region's coarse draw only when its required fine replacement is fence-complete, and restore it on eviction. Do not draw proxy and fine detail as duplicate surfaces.
4. Use the complete usable coarse proxy as the replacement-document handoff. Failure/cancel before handoff preserves the old document; partial proxy state remains explicitly Loading.
5. Keep meshoptimizer LOD construction, ~50% intermediate representations, and cross-fades deferred. If simple sampling cannot meet a fixture's stated usefulness threshold, fix the sampler or explicitly revise scope/acceptance; do not call one arbitrary chunk a proxy.
* **Verification:** Compare reordered GLB/STL/PLY fixtures against representative-proxy thresholds and complete component coverage. Verify coarse-to-fine replacement and eviction without gaps/duplicates, including cancel/failure during document handoff.

### TSK-207: Live GPU Budget, Detail Requests, and Eviction

* **Recommended Model:** Claude Code
* **Context Files to Load:** `interactive-viewer/src/graphics/DxgiBudgetMonitor.h`, `interactive-viewer/src/graphics/DxgiBudgetMonitor.cpp`, `interactive-viewer/src/app/D3D12ViewerPath.cpp`, `interactive-viewer/src/app/RenderThread.cpp`, `shared/import-broker/include/import_broker/WorkerPool.h`
* **Prompt Instructions:**
1. Wire the existing budget/request/eviction primitives into the product. Account for model geometry/textures, pending copies, and retired resources; reserve targets, fallbacks, and the coarse proxy before admitting fine detail. Apply the <=60% local-budget policy, headroom where possible, and format/UMA CPU caps from design doc 04.
2. Use verified bounds for CPU frustum culling and a bounded priority queue favoring visible/projected-error regions. Re-decode evicted fine chunks from validated source ranges inside the worker; intermediate LOD construction and persistent cache are unnecessary for this coarse/full path.
3. Keep coarse parents visible until required fine resources are ready. Shed fine geometry and texture resolution immediately when DXGI budgets shrink; deferred releases still count until fences retire. Avoid repeated failed allocations and unbounded requests during camera sweeps.
4. If the reserved set cannot fit, lower coarse density/texture resolution or show a controlled resource error. D3D12MA may supply allocation accounting, but product-owned admission/eviction policy remains authoritative.
5. Maintain a usable proxy for a model larger than available detail VRAM. Reuse/re-request source access without expanding worker path authority; integrate the final worker pool in TSK-301.
* **Verification:** Run Pressure and a large model with an artificially low detail budget on discrete and UMA paths. Assert proxy survival, cap-compliant accounted allocations/queues, fence-safe eviction, and fine-detail recovery after a camera move or budget restoration.

### TSK-208: Tier A Mesh, Point, and Material Rendering

* **Recommended Model:** Claude Code
* **Context Files to Load:** `shared/model-core/include/model_core/VertexLayouts.h`, `shared/model-core/include/model_core/MaterialPayload.h`, `import-worker/src/GltfAdapter.cpp`, `import-worker/src/PlyAdapter.cpp`, `interactive-viewer/src/app/D3D12ViewerPath.cpp`
* **Prompt Instructions:**
1. Add a depth-tested, camera-scaled round-splat point path for `PointList`/`PositionOnly_F32` and colored/normal point variants as needed. Bound screen size; do not expand every point into CPU triangles. Point-only PLY must become navigable Ready content.
2. Complete bounded vertex layouts for normals, tangents, UVs, and vertex colors; generate missing normals/tangents in the worker only when required. Preserve valid STL neutral shading, PLY colors, node transforms/instances, and double-origin precision.
3. Consume the existing metallic/roughness, normal, emissive, and unlit material fields/texture slots in the D3D12 shaders. Implement UV transforms, alpha mask/cutoff, sorted blend groups, and back-face policy respecting double-sided materials.
4. Sort/group compatible draws and reuse geometry across instances where possible. Keep feature-level-11 direct draws as the correctness path; indirect submission optimization is deferred, but Draw-heavy still must meet applicable frame targets.
* **Verification:** Run golden counts/bounds/material and pixel-readback scenes for mesh/point PLY, neutral STL, colored meshes/points, instances, PBR/unlit, normal/emissive maps, UV transforms, alpha, and double-sided surfaces. Verify point-cloud Fit/Reset/Info and no debug-layer errors.

### TSK-209: Finish the Documented glTF Feature Subset

* **Recommended Model:** Codex
* **Context Files to Load:** `import-worker/src/GltfAdapter.cpp`, `import-worker/src/DracoDecodeAdapter.cpp`, `import-worker/src/TextureTranscodeAdapter.cpp`, `import-worker/src/ImageFormatSniff.cpp`, `shared/import-broker/src/SidecarRequestServicer.cpp`, `vcpkg.json`, `.docs/design/11-decisions-and-risks.md`
* **Prompt Instructions:**
1. Complete `KHR_mesh_quantization` and bounded `EXT_meshopt_compression` decode using the pinned installed library APIs. This enables compressed-data decoding, not the deferred meshoptimizer LOD builder.
2. Add bounded WebP decode and `EXT_texture_webp`, with encoded sniff/MIME validation, aggregate image limits, color-space/mip handling, and optional-image fallback. Preserve existing Draco/KTX2/Basis paths and their independent expansion caps.
3. Verify sparse/nonlocal accessors, required versus optional extensions, local `.gltf` binary/image sidecars, and capped data URIs through the existing handle-based broker. Missing required geometry/extension data must fail rather than silently render an incomplete scene.
4. Record exact support/decode-unit limits, malformed/fuzz seeds, threat review, dependency/license/SBOM effects, and performance classification for enabled features. Remove deferred/unlinked dependencies from the shipping dependency surface when no included feature needs them, per risk R-22.
* **Verification:** Execute glTF, sidecar, compressed decode, texture, and hostile-worker suites; render A-compressed and adversarial glTF with expected counts/bounds/material snapshots. Test decode expansion limits, required-extension rejection, path traversal/reparse escape, and valid reopen after failure.

---

## Phase 3: Stabilization & Delivery

### TSK-301: Worker Reuse, Cooperative Cancellation, and Device Recovery

* **Recommended Model:** Claude Code
* **Context Files to Load:** `interactive-viewer/src/app/D3D12ImportBridge.cpp`, `interactive-viewer/src/app/RenderThread.cpp`, `shared/import-broker/src/WorkerPool.cpp`, `shared/import-broker/src/SourceFileAccess.cpp`, `shared/import-broker/src/ImportSession.cpp`, `import-worker/src/WorkerRequestDispatch.cpp`, `import-worker/src/GenerationWorker.cpp`
* **Prompt Instructions:**
1. Use a small fixed worker pool from a background coordinator. Duplicate each generation's new read-only primary/sidecar handles and output sections into the selected worker using the existing section-duplication pattern. Prove pooled mode supports real imports, progressive acknowledgements, sidecars, and fine-detail requests, not merely synthetic fixtures.
2. Close per-request handles/reset state before reuse; drain replies or terminate/replace a stale, failed, or over-budget worker. Never let a worker retained for fine-detail source access process a conflicting generation. Start/provision workers asynchronously so neither startup nor first background waits for sandbox preparation.
3. Add cooperative cancellation messages and checkpoints at scan/normalize/decode/upload boundaries. A worker must observe cancellation while parsing or blocked on batch consumption, acknowledge within 500 ms or be terminated after the bounded grace period. Cancel/replace/close wake every queue/fence wait and suppress stale publications.
4. Check failing device calls and surface asynchronous renderer-start errors. On device removal/reset, stop uploads, capture local opt-in diagnostics, rebuild the graphics lanes once, and reconstruct coarse then visible detail from retained source ranges/bounded proxy data. Repeated failure becomes a stable responsive error, without a retry loop.
5. Remove any UI-thread startup handshake or load-time mutex hold that waits on disk, worker startup, bounds work, copy submission, or GPU fences. Keep the presenting render thread running while the upload lane is delayed. Shutdown uses bounded coordinator waits and leaves no worker process or dangling detached-thread state.
* **Verification:** Execute pooled-worker, sandbox, cancellation, and hostile-worker suites. Stress real-app open/replace/cancel/close and pressure requests; assert stable handle/process/queue counts, no stale scene/metadata, responsive heartbeat, and controlled behavior under worker hangs and injected device removal/failed recovery.

### TSK-302: Performance and Memory Qualification Harness

* **Recommended Model:** Claude Code
* **Context Files to Load:** `interactive-viewer/src/app/Preview3D.cpp`, `interactive-viewer/src/graphics/FrameStats.cpp`, `interactive-viewer/src/app/RenderThread.cpp`, `shared/import-broker/include/import_broker/SandboxLauncher.h`, `.docs/design/09-quality-performance-and-security.md`
* **Prompt Instructions:**
1. Extend the existing `--frame-bench=N`/`SetBenchFrames` seams into a bounded `--benchmark` mode with a fixture path, duration/frame limit, repeat count, and machine-readable results. Explicitly attach/allocate console output or accept a result-file destination for the GUI executable.
2. Sustain rendering **on the render thread** for benchmark mode while keeping normal idle behavior and the UI message pump intact. Measure startup/background, loading UI, representative first geometry, complete coarse/verified bounds, refinement, cancel, input-to-present, heartbeat, and frame intervals.
3. Add local opt-in ETW present-event correlation/classification; keep raw intervals, automated exclusion reasons, and mean/median/p95/max/failure counts. A rolling CPU frame-time ring is insufficient for the loading/input targets.
4. Sample both viewer and worker baseline/peak private commit, mapped views separately, scratch, upload ring/queue bytes, and GPU live/pending/retired allocations. Assert named in-process limits and the **actual configured general-worker Job Object cap**, not an assumed universal 4 GiB cap (4 GiB is the original compatibility-host limit).
5. Use TSK-104 recipes and the reference-system/run metadata required by design doc 09. Apply only this pass's gates; omit warm-derived-cache, Tier B, thumbnail, and MSI claims. Emit a failing status for violated gates and document repeatable commands.
* **Verification:** Run repeated A-small/A-medium cold launches, large GLB/STL/PLY, Draw-heavy, and Pressure. Inject a copy delay, worker budget failure, and occlusion to confirm latency/limit failures and automated interval classification are actually detected.

### TSK-303: Portable Release Packaging and Dependency Closure

* **Recommended Model:** Codex
* **Context Files to Load:** `Directory.Build.targets`, `Directory.Build.props`, `Preview3D.slnx`, `interactive-viewer/Preview3D.vcxproj`, `import-worker/Preview3DImportWorker.vcxproj`, `vcpkg.json`
* **Prompt Instructions:**
1. Provide an explicit solution-level `CreatePortableRelease` entry point that builds the required Release x64 projects, waits for their app-local deployment, and packages exactly once. Verify the solution exposes this custom target; merely defining it in per-project `Directory.Build.targets` is insufficient. Do not package on every ordinary build.
2. Stage `Preview3D.exe`, `Preview3DImportWorker.exe`, their resolved runtime DLL closure (including any required non-vcpkg runtime/graphics payload), licenses/NOTICE, SBOM, and concise usage/support-limit documentation. Use a clean staging directory and allowlist, not every stale file or DLL in shared `$(OutDir)`.
3. Exclude tests, debug runtimes, thumbnail provider, compatibility host, and unrelated/deferred payloads. Pin dependency versions and identify provenance/hashes; include signing of viewer/worker and checksummed archive production in the release procedure.
4. Prove AppContainer profile/runtime read+execute provisioning works from a clean extracted directory without Visual Studio, vcpkg, developer ACLs, or admin rights. Limit ACL grants to the necessary runtime payload/profile; no source-directory access, broad user-data grants, or file association changes.
5. Record the portable distribution exception and coarse/full rendering simplifications in the design decisions/support docs. Update README, UX support text, TODO, and progress against actual results; do not mark original MSI/thumbnails/cache gates complete.
* **Verification:** `msbuild Preview3D.slnx /t:CreatePortableRelease /p:Configuration=Release /p:Platform=x64` must produce one clean archive. Extract on a clean Windows 11 x64 VM as a standard user and test all direct formats, sidecars, worker launch, errors, and cleanup. Verify dependency/license closure and archive contents.

### TSK-304: Single-Instance Activation and Accessible Existing Controls

* **Recommended Model:** Claude Code
* **Context Files to Load:** `interactive-viewer/src/app/Preview3D.cpp`, `interactive-viewer/src/ui/Chrome.cpp`, `interactive-viewer/src/ui/InfoPanel.cpp`, `interactive-viewer/src/ui/NavGizmo.cpp`, `interactive-viewer/src/graphics/D3D11On12Overlay.cpp`, `.docs/design/06-application-lifecycle-and-ipc.md`, `.docs/design/07-user-experience.md`
* **Prompt Instructions:**
1. Implement the user/session-scoped mutex and bounded, validated activation pipe described in design doc 06. A later command-line activation forwards one local supported path to the visible instance, safely replaces loading work, and exits; no daemon/tray process remains after close.
2. Keep Ctrl+O, one-file drop, command line, and existing Open With/Share behavior consistent with actual format/source metadata. Use asynchronous activation handling; no pipe/file validation waits on the UI thread.
3. Make existing non-canvas controls keyboard reachable with names/roles/states through UI Automation, preserve native error-button behavior, and announce load/error/warning state. Add high-contrast and reduce-motion handling while preserving default visuals/camera feel.
4. Verify 100/150/200% DPI, narrow layouts, Snap Layouts, Alt+Space, fullscreen versus maximize, gizmo, Info, Fit/Reset, selection/native orientation, and focus restoration after file/error dialogs. Fix regressions without adding new product surfaces.
* **Verification:** Automate second-process activation during load and after failure, malformed/oversized activation messages, and close/relaunch races. Audit keyboard/UIA/high contrast/reduce motion and mixed-DPI resize; verify every existing control remains functional while a large import is active.

### TSK-305: Release Acceptance and Honest Scope Handoff

* **Recommended Model:** Codex
* **Context Files to Load:** this manifest, `.docs/PROGRESS.md`, `.docs/TODO.md`, `.docs/design/09-quality-performance-and-security.md`, release fixture/results manifests
* **Prompt Instructions:**
1. Build clean Debug/Release x64 through the solution, execute both Catch2 suites, and run app visual/lifecycle/hostile-input qualification. Complete enabled-feature threat/fuzz/dependency/license reviews; rerun hostile-worker tests for every wire/control-protocol change.
2. Record repeatable results for every acceptance row below on the applicable performance/compatibility references. WARP smoke or missing hardware evidence cannot substitute for performance qualification.
3. Recreate and qualify the final portable archive after all fixes, including TSK-304. Run clean-standard-user extraction, Unicode-path/sidecar, process cleanup, graphics failure, and offline tests. Record archive/build/fixture hashes and material limitations.
4. Update status/docs with achieved evidence and explicitly deferred original requirements. Any failed retained gate remains blocking until fixed or deliberately changed in scope/ADRs with revised acceptance; compilation is not completion.
* **Verification:** Produce a release acceptance report and final archive whose results satisfy every retained row; no unverified row is silently marked passed.

## Scope-Limited MVP Exit Criteria

Targets use the reference systems and measurement rules in [09-quality-performance-and-security.md](./design/09-quality-performance-and-security.md). Timing values below are p95 over repeated runs; raw data, maxima, and exclusions remain available.

| Retained acceptance | Required evidence | Tasks |
| --- | --- | --- |
| Supported content | GLB/glTF local sidecars, ASCII/binary STL, ASCII and both-endian binary PLY mesh/points, glTF supported-feature/material/texture scenes match expected counts/bounds/visuals; unsupported required features fail clearly. | 202–204, 208–209, post-MVP ASCII amendment |
| Startup | Cold background <=150 ms; file-launch loading UI <=200 ms, before parsing/upload and without a synchronous worker/device wait in the UI thread. | 104, 301–302 |
| Progressive usefulness | Small complete coarse <=500 ms; medium <=2 s; large representative partial <=2 s and verified complete coarse <=5 s. A fixture defines spatial/component usefulness; a first arbitrary primitive does not pass. | 201–202, 205–206, 302 |
| Loading responsiveness | Performance reference: frame interval <=8.3 ms at 144 Hz, input-to-affected-present <=16 ms; no load-caused frame >50 ms or message heartbeat gap >100 ms. Compatibility: loading interval <=16.7 ms and input <=33 ms. | 201, 301–302, 304 |
| Ready interaction | Small/medium scenes sustain 60 fps; large proxy/resident-detail scenes >=30 fps on applicable references, including Draw-heavy and view refinement. | 207–208, 302 |
| Bounded CPU/upload memory | No source-sized private heap allocation in either process; record each process and aggregate large-run private commit beyond baseline <=1.5 GiB, mapped views separately. Scratch obeys the lower design cap; staging <=512 MiB, queues/catalogs bounded, worker stays within its configured Job cap or fails controllably. | 201, 203, 205, 301–302 |
| Out-of-core GPU behavior | Account live/pending/retired model allocations before admission; a budget drop stops detail admission and converges to the new cap through fence-safe retirement. Coarse proxy survives eviction and restored budget/view requests refine again, on discrete and UMA paths. | 206–207, 302 |
| Cancellation/recovery | Normal cancellation observation <=100 ms p95 / <=500 ms maximum outside documented library calls; worker acknowledgement <=500 ms or bounded termination. Replace/close leave no stale snapshot, leaked process/handle, deadlock, or unbounded queue; subsequent valid opens recover from import/graphics failure. | 201, 204, 301, 304 |
| Existing UX/accessibility | Real metadata and all retained controls work; keyboard/UIA, high contrast, reduce motion, 100–200% DPI, Snap/system menu/fullscreen, and later-instance activation pass. | 202, 204, 304 |
| Isolation/visual correctness | AppContainer/Job/handle policy, sidecar security, copy-then-validate, changed-source/overflow/malformed/fuzz and hostile-worker suites pass; no D3D debug/GPU-validation errors or partially copied draws. | All affected slices, 305 |
| Portable delivery | Final signed viewer/worker and runtime closure work offline for a standard user on a clean Windows 11 x64 machine; archive carries hashes, SBOM/licenses, usage/support limits, and no excluded binaries. | 303, 305 |

**Deferred original requirements:** FR-01's remaining Tier B breadth beyond ASCII STL/PLY; FR-02/NFR-09 thumbnails; FR-12/FR-13 installation/registration; FR-14/NFR-13 persistent cache and warm-cache gates; FR-15 compatibility host. Advanced image containers, full intermediate LOD generation, and indirect submission are also deferred as stated above. These remain future work, not completed original MVP gates.

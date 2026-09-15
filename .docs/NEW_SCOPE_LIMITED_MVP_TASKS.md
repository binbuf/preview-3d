# 3D Preview: Scope-Limited MVP Task Manifest

This document outlines the sequential LLM prompts required to reach a deployable MVP, restricted strictly to Tier A formats (glTF, STL, PLY) and the existing UI. Tier B formats, the persistent cache, meshoptimizer clustering, and the thumbnail provider are explicitly out of scope for this pass.

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

## Phase 2: Tier A Progressive Pipeline

### TSK-201: Progressive Display Wiring

* **Recommended Model:** Claude Code
* **Context Files to Load:** `interactive-viewer/src/app/D3D12ImportBridge.cpp`, `interactive-viewer/src/app/RenderThread.cpp`, `import-worker/src/ChunkBatchSink.cpp`
* **Prompt Instructions:**
1. Connect `ChunkBatchSink` through `D3D12ImportBridge` to the `RenderThread` so batched imports accumulate host-side.


2. Implement additive batch publication in `RenderThread` so the first geometry appears on screen before the final batch crosses the IPC boundary.


3. **Constraint:** Ensure `ChunkBatchConsumed` is correctly signaled back to the worker for flow control.




* **Verification:** `msbuild Preview3D.slnx /t:Tests_ImportIsolation`

### TSK-202: Double-Origin Bounds Verification

* **Recommended Model:** Codex
* **Context Files to Load:** `shared/model-core/include/model_core/WireFormat.h`, `interactive-viewer/src/app/D3D12ViewerPath.cpp`
* **Prompt Instructions:**
1. Add a double-precision cluster origin and local AABB/sphere to `ChunkDescriptor` while maintaining the `#pragma pack(1)` struct alignment.


2. Update the CPU bounds scan in the render thread to utilize these verified bounds.


3. **Constraint:** Explicitly skip `PositionOnly_F32` chunks (point clouds) to maintain the MVP scope constraint.




* **Verification:** `msbuild Preview3D.slnx /t:Tests_Unit`

### TSK-203: Basic WIC Image Decode

* **Recommended Model:** Codex
* **Context Files to Load:** `import-worker/src/WicImageDecodeAdapter.cpp`, `import-worker/src/TextureTranscodeAdapter.cpp`
* **Prompt Instructions:**
1. Complete the `WicImageDecodeAdapter` to support extracting base-level PNG and JPEG textures.


2. Hook the output into the standard `ImagePayloadHeader+pixelBytes` emission.


3. **Constraint:** Explicitly ignore mip-chain generation, TGA, DDS, HDR, and WebP for this slice.




* **Verification:** `msbuild Preview3D.slnx /t:Tests_ImportIsolation`

### TSK-204: Error Card Hookup

* **Recommended Model:** Codex
* **Context Files to Load:** `interactive-viewer/src/app/D3D12ImportBridge.cpp`, `interactive-viewer/src/graphics/D3D11On12Overlay.cpp`, `shared/import-broker/src/ImportSession.cpp`
* **Prompt Instructions:**
1. Map `ImportErrorCode` and `ImportStage` failures back to the D2D error card UI.


2. Display the appropriate hardcoded failure string based on the current UI specifications.


3. **Constraint:** Do not build a new diagnostic telemetry channel or warning logger; only implement the visual error card state.


* **Verification:** Run the app and drop an unsupported `.fbx` file to confirm the error card displays.

---

## Phase 3: Stabilization & Delivery

### TSK-301: Worker Pool Instance Reuse

* **Recommended Model:** Claude Code
* **Context Files to Load:** `interactive-viewer/src/app/D3D12ImportBridge.cpp`, `shared/import-broker/src/WorkerPool.cpp`, `shared/import-broker/src/SourceFileAccess.cpp`
* **Prompt Instructions:**
1. Refactor `D3D12ImportBridge` to utilize `import_broker::WorkerPool` instead of launching a fresh AppContainer worker per file open.


2. Implement a mechanism to duplicate the *new* source-file handle directly into the already-running pooled worker.


3. **Constraint:** Rely on the existing `DuplicateSectionIntoWorker` pattern for handle duplication.




* **Verification:** `msbuild Preview3D.slnx /t:Tests_ImportIsolation`

### TSK-302: BENCHMARK Performance Harness

* **Recommended Model:** Claude Code
* **Context Files to Load:** `interactive-viewer/src/app/Preview3D.cpp`, `interactive-viewer/src/graphics/FrameStats.cpp`, `interactive-viewer/src/app/RenderThread.cpp`
* **Prompt Instructions:**
1. Add a `--benchmark` command-line argument that triggers a sustained render loop (bypassing `MsgWaitForMultipleObjectsEx` idle blocking).


2. Add private-committed-bytes assertions to ensure the worker stays under the 4 GiB Job Object ceiling and the host handles pressure gracefully.


3. Output the frame stats (mean, p95) to standard out at the end of the run.




* **Verification:** Run `Preview3D.exe --benchmark <path_to_A_medium.glb>`

### TSK-303: Portable Release Packaging

* **Recommended Model:** Codex
* **Context Files to Load:** `Directory.Build.targets`
* **Prompt Instructions:**
1. Create a new MSBuild target `CreatePortableRelease` that fires after the Release build completes.
2. Package `Preview3D.exe`, `Preview3DImportWorker.exe`, and all dynamically linked vcpkg DLLs from `$(OutDir)` into a single portable `.zip` archive.
3. **Constraint:** Ensure `Preview3DThumbnailProvider.dll` and `Preview3DImportHost.exe` are excluded, as they are not part of the MVP scope.




* **Verification:** `msbuild Preview3D.slnx /t:CreatePortableRelease /p:Configuration=Release`
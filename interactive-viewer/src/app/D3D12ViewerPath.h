#pragma once

// D3D12 render path for the real Preview3D.exe, gated entirely behind the
// opt-in --d3d12 command-line flag (see Preview3D.cpp's wWinMain argument
// scan and the WM_CREATE/WM_SIZE/WM_PAINT/WM_DESTROY branches). The existing
// D3D11 Renderer stays the untouched default -- these two paths are mutually
// exclusive per window (only one swap chain can own presentation for a given
// HWND).
//
// Geometry comes from D3D12ImportBridge.h's sandboxed import pipeline, not
// Model.cpp's in-process parser. Rendering here is deliberately minimal:
// one fixed root signature/PSO/shader pair targeting
// model_core::VertexPositionNormalUv0F32's {position,normal,uv} layout (no
// materials/textures yet -- a single hardcoded albedo, hemisphere-lit by
// vertex normal only) and no chrome/D2D overlay (needs the ~750 lines of
// Renderer.cpp drawing ported onto the D3D11On12 bridge -- a separate,
// later chunk). Point-cloud (PositionOnly_F32) chunks are silently skipped
// by BeginUploadModel, not rendered.
//
// Uploads run on their own copy queue through D3D12UploadRing and publish
// through SceneSnapshot, so no load-time GPU work is submitted to or waited
// on by the direct queue. This is the shape .docs/design/
// 04-rendering-and-streaming.md:16 specifies (one direct and one copy
// queue) and what Gate 2's "direct queue records no ordinary load-time wait
// on the copy fence" exit criterion requires; the previous synchronous
// direct-queue staging path contradicted it.

#include "D3D11On12Overlay.h"
#include "D3D12CommandQueue.h"
#include "D3D12Device.h"
#include "D3D12ImportBridge.h"
#include "D3D12SwapChain.h"
#include "D3D12UploadRing.h"
#include "FrameStats.h"

#include <DirectXMath.h>
#include <platform/Generation.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct D3D12ViewerPath
{
    D3D12Device device;
    D3D12CommandQueue directQueue;
    D3D12SwapChain swapChain;

    // One allocator per swap-chain buffer so the CPU can run ahead of the
    // GPU instead of stalling to idle every frame. Per
    // .docs/design/04-rendering-and-streaming.md:32, "An
    // ID3D12CommandAllocator is reset only after the fence value of its last
    // submitted command list has completed" -- which is what fenceValue
    // records for each slot.
    static constexpr UINT kFrameCount = D3D12SwapChain::kBufferCount;
    struct FrameSlot
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        uint64_t fenceValue = 0; // 0 = nothing submitted from this slot yet
    };
    FrameSlot frames[kFrameCount];

    // Single list: a command list may be Reset onto any allocator, so unlike
    // the allocators it does not need slotting -- the same shape
    // SwapChainTests.cpp's FrameRecorder already uses.
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;

    FrameStats frameStats;

    // ADR-010 spike scaffolding. `overlayEnabled` is set from
    // --overlay-spike; when on, the scene pass leaves the back buffer in
    // RENDER_TARGET and the bridge's Release transitions it to PRESENT
    // (04-rendering-and-streaming.md:46). The synthetic content drawn is a
    // stand-in sized to the real chrome's primitive count -- porting the
    // actual ~750 lines of Renderer.cpp drawing is a later chunk, and doing
    // it before the spike answers would be building on an unmeasured
    // assumption.
    D3D11On12Overlay overlay;
    bool overlayEnabled = false;   // set before Initialize()
    // How much synthetic content to draw. 0 isolates the interop overhead
    // itself (Acquire/Release/Flush plus an empty BeginDraw/EndDraw) from
    // the cost of the D2D drawing on top of it -- two very different
    // questions, and only the first is really "what does D3D11On12 cost".
    int overlayPrimitives = 250;
    int overlayTextRuns = 40;
    double lastOverlayMs = 0.0;    // CPU ms in the last overlay pass
    double overlayTotalMs = 0.0;   // cumulative, for a mean over a run
    uint64_t overlayPasses = 0;
    // Cached rather than rebuilt per frame, matching how Renderer.cpp keeps
    // one brush and rebuilds text formats only on DPI change -- creating
    // either per frame is expensive enough to turn a cost measurement into a
    // strawman.
    //
    // One brush, not one per back buffer: the bridge now hands out a single
    // ID2D1DeviceContext retargeted at a bitmap per buffer, so device
    // resources are shared. Under the D2D 1.0 shape the spike used, each back
    // buffer had its own independent render target and a brush belonged to
    // whichever one created it, forcing a copy per buffer.
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> overlayBrush;
    // IDWriteTextFormat is a DirectWrite resource and device-independent, so
    // it survives even a target loss.
    Microsoft::WRL::ComPtr<IDWriteTextFormat> overlayTextFormat;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthBuffer;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
    // Additive textured variant: base-color-texture-or-flat-color only (no
    // metallic/roughness/normal/emissive maps this slice -- see
    // MaterialPayload's numeric fields, carried through to ImportedMaterial
    // but not consumed by either shader yet). Untextured meshes keep using
    // rootSignature/pipelineState above unchanged.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> texturedRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> texturedPipelineState;
    // kFrameCount slots of 256 bytes (D3D12's CBV alignment), bound at
    // GetGPUVirtualAddress() + frameIndex * kConstantBufferSlotBytes. One
    // shared slot was correct only while every frame stalled to idle first;
    // without that stall it is a CPU/GPU race -- the CPU would overwrite
    // frame N-1's matrix while the GPU was still reading it, which shows up
    // as an intermittently wrong camera rather than as a crash.
    static constexpr UINT kConstantBufferSlotBytes = 256;
    Microsoft::WRL::ComPtr<ID3D12Resource> frameConstantBuffer;
    std::byte* frameConstantBufferMapped = nullptr;

    struct GpuMesh
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer;
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        D3D12_INDEX_BUFFER_VIEW ibv{};
        UINT indexCount = 0;
        int textureIndex = -1; // index into textures[]; -1 = untextured PSO
    };
    struct GpuTexture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        UINT srvHeapIndex = 0;
    };

    // One model's GPU-side resources as a unit, so a completed upload can
    // replace the drawable set with a single swap and the displaced set can
    // be retired behind one fence value rather than tracked piecemeal.
    struct ModelResources
    {
        std::vector<GpuMesh> meshes;
        std::vector<GpuTexture> textures;
        // One small shader-visible CBV_SRV_UAV heap, sized to
        // textures.size() and created once per model -- not per frame.
        // Absent (nullptr) when the model has no textures.
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
        UINT srvDescriptorSize = 0;
    };

    // What RenderFrame draws. Deliberately untouched while a new model is
    // uploading, so a slow copy keeps presenting the previous model instead
    // of blanking the viewport -- 04-rendering-and-streaming.md's "the
    // direct queue ... keeps drawing the previous proxy/LOD", and the
    // behaviour Gate 2's forced-copy-delay exit criterion is written
    // against.
    ModelResources model;
    bool hasModel = false;

    // Being uploaded. These resources exist but their bytes are still in
    // flight on the copy queue, so nothing here may be drawn until
    // PollUploads() sees every one of them retire.
    ModelResources pendingModel;
    bool uploadInFlight = false;
    size_t pendingResourceCount = 0;
    platform::GenerationToken pendingToken;

    // The upload lane: a copy-typed queue plus the persistently-mapped
    // staging ring and the fence-complete publication path. This is what
    // keeps load-time GPU work off the direct queue -- Gate 2's "direct
    // queue records no ordinary load-time wait on the copy fence".
    D3D12UploadRing uploadRing;
    // Advancing this IS the cancellation signal: a superseded model's
    // publications are dropped by DrainCompletedPublications rather than
    // promoted.
    platform::GenerationSource uploadGeneration;

    // Models awaiting the fences that could still reference them. D3D12
    // command lists do not keep referenced resources alive on the
    // application's behalf, so releasing on swap would free memory the GPU
    // is still reading -- or, for a superseded upload, still *writing*.
    //
    // Both fences matter, and for different reasons:
    //   directFenceValue -- a displaced drawable model may still be
    //     referenced by frames already submitted to the direct queue;
    //   copyFenceValue   -- a superseded in-flight model has copies
    //     recorded into its destinations that have not executed yet.
    // A model released early on either timeline is a use-after-free, so
    // both must have completed.
    struct RetiredModel
    {
        ModelResources resources;
        uint64_t directFenceValue = 0;
        uint64_t copyFenceValue = 0;
    };
    std::vector<RetiredModel> retiredModels;

    // Creates the device, direct queue, swap chain (sized to `window`'s
    // current client rect), the per-frame command allocators and the shared
    // command list, the upload allocator/list, the depth buffer, the root
    // signature/PSO, and the slotted per-frame constant buffer.
    bool Initialize(HWND window, std::wstring& error);

    // Precondition enforced internally via WaitForIdle() first -- matches
    // D3D12SwapChain::Resize's own documented precondition. Also rebuilds
    // the depth buffer at the new size.
    bool Resize(int width, int height, std::wstring& error);

    // Clears to the design doc's Gate 1 "immediate #1C1C1E" idle background
    // and presents, with no geometry -- used before a model is loaded and on
    // import failure (there is no D2D error card on this path yet).
    void RenderClearFrame();

    // Draws every uploaded mesh with the supplied view-projection matrix.
    // One DrawIndexedInstanced per mesh -- never assumes exactly one chunk.
    //
    // Takes the matrix rather than the Camera deliberately: the camera is
    // shared with the UI thread under a lock, and holding that lock across a
    // whole frame -- including BeginFrame's fence and frame-latency waits --
    // would block input for the length of a GPU frame. The caller ticks the
    // camera and computes this under the lock, then releases it and renders.
    void RenderFrame(const DirectX::XMFLOAT4X4& viewProjection);

    // Uploads every TriangleList/PositionNormalUv0_F32 mesh in `importedMeshes`
    // as a DEFAULT-heap vertex+index buffer pair (synchronous staging-buffer
    // copy). Meshes with any other topology/layout (e.g. a PLY point cloud)
    // are silently skipped -- point-cloud rendering is a later slice. `images`
    // are uploaded as DEFAULT-heap Texture2D resources (one SRV each, in a
    // fresh shader-visible heap sized to images.size()); a mesh whose
    // material resolves to a base-color image gets `textureIndex` set and
    // renders through the textured PSO, sampling that texture and
    // multiplying it into the existing hemisphere/Lambertian shade -- other
    // MaterialPayload fields (metallic/roughness/emissive factors, other
    // texture slots) are carried through `materials` but not consumed by
    // either shader yet, a deliberate scope narrowing for this slice.
    // Returns false (with `error` set) if no renderable mesh resulted.
    //
    // Asynchronous: this creates the destination resources and queues every
    // copy onto the upload ring's copy queue, then returns without waiting.
    // The previously uploaded model keeps drawing until PollUploads()
    // observes the whole batch retire. Supersedes any upload already in
    // flight by advancing uploadGeneration, which makes the abandoned one's
    // publications stale rather than merely unwanted.
    bool BeginUploadModel(const std::vector<d3d12_import_bridge::ImportedMesh>& importedMeshes,
                           const std::vector<d3d12_import_bridge::ImportedMaterial>& importedMaterials,
                           const std::vector<d3d12_import_bridge::ImportedImage>& importedImages,
                           std::wstring& error);

    // Drains the ring's fence-complete publications and, once every
    // resource of the in-flight model is ready, swaps it in as the drawable
    // one and retires the displaced set behind the current direct fence.
    // Returns true exactly on that transition, so a caller can post its
    // "upload complete" notification once. Cheap; call every frame.
    bool PollUploads();

    bool UploadInFlight() const noexcept { return uploadInFlight; }

    // Releases retired models whose direct fence has completed, and lets
    // the ring reclaim staging space. Cheap; call every frame.
    void ReclaimRetired();

    // Releases the currently uploaded GPU buffers, if any. Waits for the GPU
    // to be idle first -- callers must not still be mid-frame.
    void ClearModel();

    // Bounded wait until every outstanding frame slot and any upload has
    // completed. Called before Resize, before releasing GPU resources, and on
    // WM_DESTROY -- GPU work must be known-idle before this struct's
    // destructor releases the D3D12 objects; RAII alone doesn't order that.
    //
    // Deliberately NOT called per frame any more: that is what serialized the
    // whole path. Per-frame waiting is now the frame-latency waitable object
    // plus the current slot's own fence value.
    void WaitForIdle();

private:
    // The per-frame wait that replaced WaitForIdle(): blocks on the swap
    // chain's frame-latency waitable object, then on this slot's own fence so
    // its allocator is only reset once its last submission has completed.
    // Returns the back-buffer index to render into.
    UINT BeginFrame();
    // Present, then signal and record the fence value into the slot.
    void EndFrame(UINT frameIndex);
    // Spike-only: draws a synthetic overlay of roughly the real chrome's
    // primitive count, so the measured cost means something. Returns the
    // CPU milliseconds spent between Acquire and Flush.
    double DrawSpikeOverlay(UINT frameIndex);
    // Moves resources the copy queue may still be writing into onto the
    // retire list behind the ring's current submitted fence, instead of
    // letting them destruct here. Flushes first, so that fence value
    // genuinely covers copies recorded into them.
    void RetireStagedResources(ModelResources&& resources);
    // Retires whatever upload is still in flight, for when a new one
    // supersedes it or the model is being torn down.
    void RetireInFlightUpload();
    bool CreateDepthBuffer(UINT width, UINT height, std::wstring& error);
    bool CreatePipeline(std::wstring& error);
    bool CreateTexturedPipeline(std::wstring& error);
    bool CreateFrameConstantBuffer(std::wstring& error);
    // Creates a DEFAULT-heap buffer in COMMON and queues its copy onto the
    // upload ring. No explicit barrier to VERTEX_AND_CONSTANT_BUFFER /
    // INDEX_BUFFER: a copy queue cannot record one, and none is needed --
    // the buffer promotes implicitly to COPY_DEST for the copy, decays back
    // to COMMON when the copy queue's ExecuteCommandLists completes, then
    // promotes again on the direct queue at first use. Confirmed
    // empirically for buffers on this exact path in Gate 2 workstream B
    // slice 3 (see .docs/PROGRESS.md's upload-ring risk table).
    bool CreateAndQueueBuffer(const void* data, uint64_t sizeBytes, uint32_t clusterId,
                               Microsoft::WRL::ComPtr<ID3D12Resource>& outBuffer, std::wstring& error);
    // Creates one image's DEFAULT-heap Texture2D (mip 0 only, in COMMON for
    // the same implicit-promotion reason as buffers), writes its SRV into
    // `heap` at heapIndex, and queues its copy onto the upload ring. The
    // row repitching the wire format needs -- its rows are tightly packed
    // and do not satisfy D3D12's 256-byte staging pitch -- now lives in
    // D3D12UploadRing::UploadTexture rather than here.
    //
    // The SRV is written at queue time deliberately: creating a descriptor
    // only describes the resource, so it does not require the copy to have
    // landed.
    bool CreateAndQueueTexture(const d3d12_import_bridge::ImportedImage& image, UINT heapIndex,
                                ID3D12DescriptorHeap& heap, UINT descriptorSize, uint32_t clusterId,
                                Microsoft::WRL::ComPtr<ID3D12Resource>& outTexture, std::wstring& error);
};

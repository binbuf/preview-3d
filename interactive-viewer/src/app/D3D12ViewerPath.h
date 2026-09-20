#pragma once

// Exclusive render path for Preview3D.exe, owned by RenderThread. Device
// creation, presentation, resize, and shutdown all happen on that thread.
// Renderer.cpp is retained for its camera implementation and deprecated
// scene renderer.
//
// Geometry comes from D3D12ImportBridge.h's sandboxed import pipeline, not
// Model.cpp's in-process parser. Triangle meshes support the complete
// position/normal/UV/tangent/color layout and normalized material contract;
// PointList chunks use depth-tested, camera-scaled round splats. Chrome is
// painted through the D3D11On12/Direct2D bridge.
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
    HRESULT lastPresentResult = E_PENDING;
    D3D12Device device;
    D3D12Device::CreateOptions deviceOptions;
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

    // Chrome is always painted over the scene on the shared direct queue.
    D3D11On12Overlay overlay;
    double lastOverlayMs = 0.0;
    double overlayTotalMs = 0.0;
    uint64_t overlayPasses = 0;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthBuffer;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> blendPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> gridPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pointPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> coloredPointPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> positionOnlyPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> wireframePipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> positionOnlyWireframePipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> completeVertexPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource> pickTarget;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> pickRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> pickReadback;
    uint64_t pickFence = 0;
    uint32_t lastPickedId = 0;
    bool pickInFlight = false;
    int pickX = -1, pickY = -1;
    // Complete material variant: base-color, metallic/roughness, normal, and
    // emissive maps plus normalized factors, alpha, unlit, UV transform, and
    // double-sided behavior. Legacy position-only meshes keep using
    // rootSignature/pipelineState above unchanged.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> texturedRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> texturedPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> texturedMirroredPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> texturedDoubleSidedPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> texturedBlendPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> texturedBlendMirroredPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> texturedBlendDoubleSidedPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> texturedWireframePipelineState;
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
        uint64_t vertexAllocationBytes = 0, indexAllocationBytes = 0;
        uint32_t lastVisibleFrame = 0;
        float viewPriority = 0;
        float viewDepth = 0;
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        D3D12_INDEX_BUFFER_VIEW ibv{};
        uint32_t chunkId = 0;
        uint32_t materialChunkId = 0;
        uint32_t sourceMeshId = 0;
        uint32_t sourceNodeId = 0;
        uint32_t instanceId = 0; // stable scene/picking id; 0 for legacy world-baked draws
        model_core::ChunkDescriptor sourceGeometry{}; // retained bounded source provenance for re-decode
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> textureHeap;
        UINT textureDescriptorSize = 0;
        UINT indexCount = 0;
        UINT vertexCount = 0;
        bool points = false;
        bool positionOnly = false;
        bool completeVertex = false;
        bool drawEnabled = true;
        double origin[3]{};
        double instanceTransform[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        double instanceBoundsMin[3]{}, instanceBoundsMax[3]{};
        bool mirrored = false;
        int textureIndex = -1; // base-color index retained for pressure compatibility
        int textureIndices[4]{-1,-1,-1,-1};
        Microsoft::WRL::ComPtr<ID3D12Resource> neutralResources[4];
        UINT neutralDescriptorBase = 0;
        model_core::MaterialPayload material{};
        bool hasMaterial = false;
    };
    struct GpuTexture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint32_t chunkId = 0;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        UINT descriptorSize = 0;
        UINT srvHeapIndex = 0;
        uint64_t allocationBytes = 0;
    };

    // One model's GPU-side resources as a unit, so a completed upload can
    // replace the drawable set with a single swap and the displaced set can
    // be retired behind one fence value rather than tracked piecemeal.
    struct ModelResources
    {
        bool coarseComplete = false;
        uint64_t coarseAllocationBytes = 0;
        std::vector<GpuMesh> meshes;
        std::vector<GpuTexture> textures;
        std::vector<GpuTexture> neutralTextures;
        std::vector<GpuTexture> fallbackTextures;
        // Descriptor heaps displaced by a progressive catalog rebuild remain
        // alive until every direct-queue frame that could reference them retires.
        std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> retainedDescriptorHeaps;
        // One shader-visible catalog for all current images plus the four
        // neutral slots. Progressive publications rebuild it atomically.
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
    static void UpdateCoarseVisibility(ModelResources& resources);
    void EvictFineChunks(std::span<const uint32_t> identities);
    void RetirePreviewChunks(ModelResources& resources);
    uint64_t AccountedAllocationBytes(const ModelResources* extra = nullptr) const;
    static uint64_t EstimateUploadBytes(ID3D12Device* device,
        std::span<const d3d12_import_bridge::ImportedMesh> meshes,
        std::span<const d3d12_import_bridge::ImportedImage> images,
        bool includeMaterialFallbacks = true);
    bool RebuildMaterialDescriptors(ModelResources& resources, std::wstring& error);
    void ShedTextureDetail();
    bool hasModel = false;
    double sceneOrigin[3]{};
    model_core::UpAxisId sourceUpAxis = model_core::UpAxisId::Unknown;
    DirectX::XMFLOAT3 modelBoundsMin{};
    DirectX::XMFLOAT3 modelBoundsMax{};
    bool haveModelBounds = false;

    // Being uploaded. These resources exist but their bytes are still in
    // flight on the copy queue, so nothing here may be drawn until
    // PollUploads() sees every one of them retire.
    ModelResources pendingModel;
    bool uploadInFlight = false;
    size_t pendingResourceCount = 0;
    platform::GenerationToken pendingToken;
    std::function<bool()> uploadIsCancelled; // coordinator-owned, polled at allocation/mip boundaries
    model_core::ImportErrorCode uploadErrorCode = model_core::ImportErrorCode::UploadFailure;

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
    // import failure, with chrome and the current state card overlaid.
    void RenderClearFrame(const DirectX::XMFLOAT4& orientation, const OverlayFrame& chrome);

    // Draws every uploaded mesh with the supplied view-projection matrix.
    // One DrawIndexedInstanced per mesh -- never assumes exactly one chunk.
    //
    // Takes the matrix rather than the Camera deliberately: the camera is
    // shared with the UI thread under a lock, and holding that lock across a
    // whole frame -- including BeginFrame's fence and frame-latency waits --
    // would block input for the length of a GPU frame. The caller ticks the
    // camera and computes this under the lock, then releases it and renders.
    void RenderFrame(const DirectX::XMFLOAT4X4& viewProjection, const DirectX::XMFLOAT4& orientation,
                     const OverlayFrame& chrome, const double cameraTarget[3], const DirectX::XMFLOAT4& eyeSelection);
    bool PollPick(bool& hit);
    uint32_t LastPickedId() const noexcept { return lastPickedId; }

    // Uploads TriangleList meshes in complete, normal/UV, or position-only
    // layouts and PointList meshes in complete or position-only layouts on
    // the upload coordinator. Geometry keeps its validated double origin;
    // points need no index buffer. `images` become DEFAULT-heap Texture2D
    // resources in one shader-visible heap alongside neutral map fallbacks.
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
                           std::wstring& error,
                           std::span<const d3d12_import_bridge::ImportedNode> importedNodes = {},
                           std::span<const d3d12_import_bridge::ImportedInstance> importedInstances = {});

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
    // Records CPU time for the real chrome, including Acquire/Release/Flush.
    void DrawChrome(UINT frameIndex, const DirectX::XMFLOAT4& orientation, const OverlayFrame& chrome);
    // Moves resources the copy queue may still be writing into onto the
    // retire list behind the ring's current submitted fence, instead of
    // letting them destruct here. Flushes first, so that fence value
    // genuinely covers copies recorded into them.
    void RetireStagedResources(ModelResources&& resources);
    // Retires whatever upload is still in flight, for when a new one
    // supersedes it or the model is being torn down.
    void RetireInFlightUpload();
    bool CreateDepthBuffer(UINT width, UINT height, std::wstring& error);
    bool CreatePickTarget(UINT width, UINT height, std::wstring& error);
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
                               Microsoft::WRL::ComPtr<ID3D12Resource>& outBuffer, std::wstring& error,
                               uint64_t destinationOffset = 0);
    // Creates one image's immutable DEFAULT-heap mip chain (in COMMON for
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

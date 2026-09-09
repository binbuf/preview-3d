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
// vertex normal only), a plain synchronous per-buffer upload (no
// D3D12UploadRing -- that primitive is for progressive/streaming multi-chunk
// upload, unnecessary complexity for one static mesh), and no chrome/D2D
// overlay (needs D3D11-on-12 interop or a parallel D2D path -- a separate,
// later slice). Point-cloud (PositionOnly_F32) chunks are silently skipped
// by UploadModel, not rendered.

#include "D3D11On12Overlay.h"
#include "D3D12CommandQueue.h"
#include "D3D12Device.h"
#include "D3D12ImportBridge.h"
#include "D3D12SwapChain.h"
#include "FrameStats.h"

#include <DirectXMath.h>
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

    // Uploads keep their synchronous semantics -- their staging buffers are
    // locals released at return, which is only safe because they block -- but
    // must not touch a render slot's allocator, since resetting one whose
    // frame is still in flight is exactly what the slots above exist to
    // prevent. This mirrors D3D12UploadRing, which already owns its own
    // queue, allocator and fence value (D3D12UploadRing.h:150-154).
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> uploadAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> uploadList;
    uint64_t uploadFence = 0;

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
    // either per frame is expensive enough to turn this measurement into a
    // strawman.
    //
    // The brush is per target, not shared: CreateDxgiSurfaceRenderTarget
    // gives one independent ID2D1RenderTarget per back buffer, and a brush
    // belongs to the target that created it. (Renderer.cpp has exactly one
    // target, so a single brush is correct there.) That per-buffer
    // duplication is an argument for moving to ID2D1Device/ID2D1DeviceContext
    // plus a bitmap per buffer when the real chrome is ported -- device-level
    // resources are then shared, and ID2D1DeviceContext is itself an
    // ID2D1RenderTarget so the drawing code is unaffected.
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> overlayBrushes[D3D12SwapChain::kBufferCount];
    // IDWriteTextFormat is a DirectWrite resource, device-independent, so
    // this one really is shareable across targets.
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
    std::vector<GpuMesh> meshes;
    bool hasModel = false;

    struct GpuTexture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        UINT srvHeapIndex = 0;
    };
    std::vector<GpuTexture> textures;
    // One small shader-visible CBV_SRV_UAV heap, sized to textures.size()
    // and (re)created once per UploadModel call -- not per-frame. Absent
    // (nullptr) when the current model has no textures.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
    UINT srvDescriptorSize = 0;

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
    // Replaces any previously uploaded model first.
    bool UploadModel(const std::vector<d3d12_import_bridge::ImportedMesh>& importedMeshes,
                      const std::vector<d3d12_import_bridge::ImportedMaterial>& importedMaterials,
                      const std::vector<d3d12_import_bridge::ImportedImage>& importedImages,
                      std::wstring& error);

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
    // Bounded wait on the upload lane's own fence only. Rendering is
    // unaffected -- that is the point of the separate allocator.
    void WaitForUpload();
    bool CreateDepthBuffer(UINT width, UINT height, std::wstring& error);
    bool CreatePipeline(std::wstring& error);
    bool CreateTexturedPipeline(std::wstring& error);
    bool CreateFrameConstantBuffer(std::wstring& error);
    bool UploadOneBuffer(const void* data, uint64_t sizeBytes, D3D12_RESOURCE_STATES finalState,
                          Microsoft::WRL::ComPtr<ID3D12Resource>& outBuffer, std::wstring& error);
    // Uploads one image's pixel bytes (mip 0 only, per PixelFormats.h's
    // tightly-packed layout) as a DEFAULT-heap Texture2D and writes its SRV
    // into srvHeap at heapIndex. Uses ID3D12Device::GetCopyableFootprints to
    // build a correctly row-pitch-aligned (256-byte) UPLOAD-heap staging
    // buffer per row -- the wire format's tightly-packed rows do not
    // satisfy D3D12's upload-heap pitch requirement, so a straight memcpy
    // of the source bytes would be wrong here.
    bool UploadOneTexture(const d3d12_import_bridge::ImportedImage& image, UINT heapIndex,
                          Microsoft::WRL::ComPtr<ID3D12Resource>& outTexture, std::wstring& error);
};

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

#include "D3D12CommandQueue.h"
#include "D3D12Device.h"
#include "D3D12ImportBridge.h"
#include "D3D12SwapChain.h"
#include "Renderer.h" // for Camera -- pure DirectXMath, no D3D11 coupling

#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

struct D3D12ViewerPath
{
    D3D12Device device;
    D3D12CommandQueue directQueue;
    D3D12SwapChain swapChain;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    uint64_t lastFrameFence = 0;

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
    // One persistently-mapped upload-heap CB reused every frame -- plain
    // per-frame Map/memcpy, no ring, matching this slice's "fully
    // synchronous" scope.
    Microsoft::WRL::ComPtr<ID3D12Resource> frameConstantBuffer;
    void* frameConstantBufferMapped = nullptr;

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
    // current client rect), the single command allocator/list this path
    // reuses every frame, the depth buffer, the root signature/PSO, and the
    // per-frame constant buffer.
    bool Initialize(HWND window, std::wstring& error);

    // Precondition enforced internally via WaitForIdle() first -- matches
    // D3D12SwapChain::Resize's own documented precondition. Also rebuilds
    // the depth buffer at the new size.
    bool Resize(int width, int height, std::wstring& error);

    // Clears to the design doc's Gate 1 "immediate #1C1C1E" idle background
    // and presents, with no geometry -- used before a model is loaded and on
    // import failure (there is no D2D error card on this path yet).
    void RenderClearFrame();

    // Draws every uploaded mesh with the camera's current view/projection.
    // One DrawIndexedInstanced per mesh -- never assumes exactly one chunk.
    void RenderFrame(const Camera& camera, float aspect);

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

    // Bounded wait on lastFrameFence. Called before Resize, before releasing
    // GPU resources, and on WM_DESTROY -- GPU work must be known-idle before
    // this struct's destructor releases the D3D12 objects; RAII alone
    // doesn't order that.
    void WaitForIdle();

private:
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

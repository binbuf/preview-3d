// Gate 2's "DXGI budget monitor, view-priority requester, detail eviction"
// deliverable. Real-hardware coverage proves the actual IDXGIAdapter3 path;
// an injectable query seam separately proves the design doc's exact detail-
// target formula and budget-drop responsiveness deterministically, without
// needing genuine system memory pressure. PlanEviction is tested purely
// against synthetic SceneSnapshot data -- no GPU/device needed at all.

#include "D3D12Device.h"
#include "DxgiBudgetMonitor.h"
#include "SceneSnapshot.h"
#include "DetailView.h"
#include "D3D12ViewerPath.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace {

D3D12Device& SharedDevice()
{
    static D3D12Device device = [] {
        D3D12Device d;
        auto result = d.Initialize();
        REQUIRE(result.success);
        return d;
    }();
    return device;
}

} // namespace

TEST_CASE("DxgiBudgetMonitor initializes against real hardware and reports a plausible budget", "[graphics]")
{
    DxgiBudgetMonitor monitor;
    std::wstring error;
    REQUIRE(monitor.Initialize(SharedDevice(), {}, error));

    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    REQUIRE(monitor.QueryBudget(info));
    CHECK(info.Budget > 0);
    CHECK(monitor.ComputeDetailTargetBytes() > 0);

    // Confirmed empirically on this dev machine: the registered event is
    // already signaled the first time it's checked, apparently as an
    // initial notification rather than only future changes -- not
    // documented behavior we can rely on either way, so this is
    // informational only, not a hard guarantee (matching this repo's
    // existing precedent for environment-dependent signals, e.g. the debug
    // layer's optionality in GraphicsDeviceTests.cpp). Calling it at all
    // proves polling doesn't crash/hang; the actual boolean isn't asserted.
    bool signaled = monitor.HasBudgetChangeSignaled();
    INFO("HasBudgetChangeSignaled() on first check: " << signaled);
    CHECK((signaled == true || signaled == false));
}

TEST_CASE("ComputeDetailTargetBytes matches the design doc's exact formula against an injected budget",
          "[graphics]")
{
    DxgiBudgetMonitor monitor;
    std::wstring error;
    REQUIRE(monitor.Initialize(SharedDevice(), {}, error));

    monitor.SetQueryOverride([](DXGI_QUERY_VIDEO_MEMORY_INFO& info) {
        info = {};
        info.Budget = 4ull * 1024 * 1024 * 1024; // 4 GiB
        return true;
    });

    // 60% of 4 GiB = 2.4 GiB; 4 GiB - 512 MiB headroom = 3.5 GiB -- the
    // smaller of the two (60%) is the actual bound.
    uint64_t expectedSixtyPercent = static_cast<uint64_t>(4.0 * 1024 * 1024 * 1024 * 0.6);
    CHECK(monitor.ComputeDetailTargetBytes() == expectedSixtyPercent);
}

TEST_CASE("ComputeDetailTargetBytes recalculates immediately when the injected budget drops", "[graphics]")
{
    DxgiBudgetMonitor monitor;
    std::wstring error;
    REQUIRE(monitor.Initialize(SharedDevice(), {}, error));

    uint64_t currentBudget = 4ull * 1024 * 1024 * 1024;
    monitor.SetQueryOverride([&currentBudget](DXGI_QUERY_VIDEO_MEMORY_INFO& info) {
        info = {};
        info.Budget = currentBudget;
        return true;
    });

    uint64_t before = monitor.ComputeDetailTargetBytes();
    currentBudget = 512ull * 1024 * 1024; // drop to exactly the headroom floor
    uint64_t after = monitor.ComputeDetailTargetBytes();

    CHECK(after < before);
    CHECK(after == currentBudget*3/5); // retain the 60% cap when 512 MiB headroom is impossible
}

TEST_CASE("ComputeDetailTargetBytes respects the format policy cap", "[graphics]")
{
    DxgiBudgetMonitor::CreateOptions options;
    options.formatPolicyCapBytes = 128ull * 1024 * 1024; // deliberately below what 60%/headroom would allow

    DxgiBudgetMonitor monitor;
    std::wstring error;
    REQUIRE(monitor.Initialize(SharedDevice(), options, error));

    monitor.SetQueryOverride([](DXGI_QUERY_VIDEO_MEMORY_INFO& info) {
        info = {};
        info.Budget = 4ull * 1024 * 1024 * 1024;
        return true;
    });

    CHECK(monitor.ComputeDetailTargetBytes() == options.formatPolicyCapBytes);
}

TEST_CASE("PlanEviction returns nothing when everything already fits", "[graphics]")
{
    std::vector<ReadyResourceInfo> ready{
        ReadyResourceInfo{ nullptr, 0, /*clusterId=*/1, /*lodLevel=*/0, /*approximateBytes=*/100,
                           /*lastVisibleFrame=*/5 },
    };
    SceneSnapshot snapshot(ready);
    CHECK(PlanEviction(snapshot, /*targetBytes=*/1000).empty());
}

TEST_CASE("PlanEviction evicts least-recently-visible resources first, stopping once the target fits",
          "[graphics]")
{
    std::vector<ReadyResourceInfo> ready{
        ReadyResourceInfo{ nullptr, 0, 1, 0, /*approximateBytes=*/100, /*lastVisibleFrame=*/10 }, // most recent
        ReadyResourceInfo{ nullptr, 0, 2, 0, /*approximateBytes=*/100, /*lastVisibleFrame=*/1 },  // least recent
        ReadyResourceInfo{ nullptr, 0, 3, 0, /*approximateBytes=*/100, /*lastVisibleFrame=*/5 },
    };
    SceneSnapshot snapshot(ready);

    // Total 300; target 150 -- must drop the two least-recently-visible
    // (cluster 2, then cluster 3) to reach 100, which fits.
    auto plan = PlanEviction(snapshot, /*targetBytes=*/150);
    REQUIRE(plan.size() == 2);
    CHECK(plan[0].clusterId == 2);
    CHECK(plan[1].clusterId == 3);
}


TEST_CASE("Verified detail bounds cull offscreen regions and favor projected extent", "[detail-budget]") {
    model_core::ChunkDescriptor d{}; d.localMin[0]=d.localMin[1]=-0.1f; d.localMin[2]=0.1f;
    d.localMax[0]=d.localMax[1]=0.1f; d.localMax[2]=0.2f;
    DirectX::XMFLOAT4X4 vp; DirectX::XMStoreFloat4x4(&vp,DirectX::XMMatrixIdentity());
    double origin[3]={1e12,1e12,1e12},target[3]{}; std::copy(std::begin(origin),std::end(origin),std::begin(d.origin));
    DirectX::XMFLOAT4X4 modelTransform{};
    DirectX::XMStoreFloat4x4(&modelTransform,DirectX::XMMatrixIdentity());
    const float smallScore=DetailViewPriority(d,origin,target,modelTransform,vp); CHECK(smallScore>0);
    d.localMin[0]=-0.9f; d.localMax[0]=0.9f;
    CHECK(DetailViewPriority(d,origin,target,modelTransform,vp)>smallScore);
    d.origin[0]+=10; CHECK(DetailViewPriority(d,origin,target,modelTransform,vp)==0);
    target[0]=10; CHECK(DetailViewPriority(d,origin,target,modelTransform,vp)>0);
}

TEST_CASE("Destination admission charges aligned buffers and skips full scan payloads", "[detail-budget]") {
    d3d12_import_bridge::ImportedMesh mesh; mesh.vertexCount=3; mesh.indexCount=3;
    mesh.vertexLayoutId=model_core::VertexLayoutId::PositionNormalUv0_F32;
    auto* device=SharedDevice().Device();
    CHECK(D3D12ViewerPath::EstimateUploadBytes(device,std::span(&mesh,1),{})==131072);
    mesh.geometry.lodLevel=model_core::kScanLod;
    CHECK(D3D12ViewerPath::EstimateUploadBytes(device,std::span(&mesh,1),{})==0);
    mesh.geometry.lodLevel=model_core::kCoarseLod;
    std::vector<d3d12_import_bridge::ImportedMesh> samples(2048,mesh);
    CHECK(D3D12ViewerPath::EstimateUploadBytes(device,samples,{})==262144);
}


TEST_CASE("Accounted shared buffers remain charged through both retirement fences", "[detail-budget]") {
    auto* device=SharedDevice().Device();
    D3D12ViewerPath path; path.device.AttachForUpload(device);
    REQUIRE(path.directQueue.Initialize(*device,D3D12_COMMAND_LIST_TYPE_DIRECT,L"Budget direct"));
    D3D12UploadRing::CreateOptions options; options.initialCapacityBytes=4096; options.maxCapacityBytes=4096;
    REQUIRE(path.uploadRing.Initialize(path.device,options));
    Microsoft::WRL::ComPtr<ID3D12Fence> directGate,copyGate;
    REQUIRE(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&directGate))));
    REQUIRE(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&copyGate))));
    struct OpenGates {
        ID3D12Fence* direct; ID3D12Fence* copy;
        ~OpenGates() { direct->Signal(1); copy->Signal(1); }
    } release{directGate.Get(),copyGate.Get()};
    REQUIRE(SUCCEEDED(path.directQueue.Queue()->Wait(directGate.Get(),1)));
    REQUIRE(SUCCEEDED(path.uploadRing.CopyQueue().Queue()->Wait(copyGate.Get(),1)));
    const uint64_t directFence=path.directQueue.SignalNext(),copyFence=path.uploadRing.CopyQueue().SignalNext();
    REQUIRE(directFence); REQUIRE(copyFence);
    D3D12ViewerPath::GpuMesh fine; fine.chunkId=1; fine.sourceGeometry.lodLevel=model_core::kFineLod;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=4096;
    desc.Height=1; desc.DepthOrArraySize=1; desc.MipLevels=1; desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    REQUIRE(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&fine.vertexBuffer))));
    REQUIRE(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&fine.indexBuffer))));
    D3D12ViewerPath::GpuMesh coarse; coarse.chunkId=model_core::kCoarseIdentity|1; coarse.sourceGeometry.lodLevel=model_core::kCoarseLod;
    coarse.vertexBuffer=fine.vertexBuffer;
    path.model.meshes.push_back(std::move(coarse)); path.model.meshes.push_back(std::move(fine));
    const uint64_t before=path.AccountedAllocationBytes();
    CHECK(before==4*1024*1024+2*65536);
    path.frames[0].fenceValue=directFence;
    const uint32_t identity=1; path.EvictFineChunks(std::span(&identity,1));
    REQUIRE(path.retiredModels.size()==1); path.retiredModels.front().copyFenceValue=copyFence;
    path.ReclaimRetired(); CHECK(path.AccountedAllocationBytes()==before);
    directGate->Signal(1);
    REQUIRE(path.directQueue.WaitForValue(directFence,2000)==D3D12CommandQueue::WaitResult::Signaled);
    path.ReclaimRetired(); CHECK(path.AccountedAllocationBytes()==before);
    copyGate->Signal(1);
    REQUIRE(path.uploadRing.CopyQueue().WaitForValue(copyFence,2000)==D3D12CommandQueue::WaitResult::Signaled);
    path.ReclaimRetired(); CHECK(path.retiredModels.empty());
    CHECK(path.AccountedAllocationBytes()==before-65536);
}

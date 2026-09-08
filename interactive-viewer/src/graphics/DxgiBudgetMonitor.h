#pragma once

// DXGI video-memory budget monitor + a plan-only eviction planner, per
// .docs/design/04-rendering-and-streaming.md's "Video-memory budget and
// residency" section. Re-acquires the adapter D3D12Device selected (it
// doesn't retain one after Initialize()) via GetAdapterLuid() +
// EnumAdapterByLuid(), then QIs to IDXGIAdapter3 for
// QueryVideoMemoryInfo/RegisterVideoMemoryBudgetChangeNotificationEvent.
//
// Scoping note: "view-priority" here is recency (a caller-supplied
// lastVisibleFrame on ReadyResourceInfo) plus size, not a camera/frustum
// projected-screen-error system -- no renderer/camera exists in this
// codebase yet to feed one. PlanEviction only ever produces a drop list;
// it never calls Release() on a GPU resource itself, matching the design
// doc's "removes a chunk from a new snapshot first" ordering, which is the
// upload ring/future renderer's job, not this class's.

#include "SceneSnapshot.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <platform/Win32Handle.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class D3D12Device;

class DxgiBudgetMonitor
{
public:
    struct CreateOptions
    {
        // "no more than the format generation's CPU/GPU policy cap" --
        // UINT64_MAX (no extra cap) unless a caller sets one.
        uint64_t formatPolicyCapBytes = UINT64_MAX;
    };

    // Injectable seam: real usage leaves this unset (queries the real
    // IDXGIAdapter3 via QueryBudget's default path); a test can call
    // SetQueryOverride to simulate a budget value -- including a drop --
    // deterministically, without needing genuine system memory pressure.
    using QueryFn = std::function<bool(DXGI_QUERY_VIDEO_MEMORY_INFO&)>;

    DxgiBudgetMonitor() = default;
    DxgiBudgetMonitor(const DxgiBudgetMonitor&) = delete;
    DxgiBudgetMonitor& operator=(const DxgiBudgetMonitor&) = delete;
    DxgiBudgetMonitor(DxgiBudgetMonitor&&) = default;
    DxgiBudgetMonitor& operator=(DxgiBudgetMonitor&&) = default;
    ~DxgiBudgetMonitor();

    bool Initialize(D3D12Device& device, const CreateOptions& options, std::wstring& error);

    // Real-hardware query path (DXGI_MEMORY_SEGMENT_GROUP_LOCAL, node 0).
    // Bypassed entirely when a QueryFn override is set.
    bool QueryBudget(DXGI_QUERY_VIDEO_MEMORY_INFO& outInfo) const;

    void SetQueryOverride(QueryFn fn) { queryOverride_ = std::move(fn); }

    // Recomputes the detail target fresh from the current budget query
    // every call (never cached), per the design doc's exact formula: no
    // more than 60% of the reported local budget, at least 512 MiB of
    // headroom when the budget permits, no more than the format policy
    // cap. Returns 0 if the budget query itself fails (conservative: no
    // budget info, no detail).
    uint64_t ComputeDetailTargetBytes() const;

    // Non-blocking check of the registered budget-change event.
    bool HasBudgetChangeSignaled() const;

    HANDLE BudgetChangeEvent() const noexcept { return budgetChangeEvent_.get(); }

private:
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3_;
    platform::Win32Handle budgetChangeEvent_;
    DWORD budgetChangeCookie_ = 0;
    CreateOptions options_;
    QueryFn queryOverride_;
};

struct EvictionCandidate
{
    uint32_t clusterId = 0;
    uint32_t lodLevel = 0;
};

// Given `snapshot`'s ready resources and a byte budget, returns the
// minimal ordered list of (clusterId, lodLevel) to drop -- lowest
// lastVisibleFrame (least recently visible) first -- so the remaining
// resources' summed approximateBytes fits within targetBytes. A plan
// only: never calls Release() on any GPU resource itself.
std::vector<EvictionCandidate> PlanEviction(const SceneSnapshot& snapshot, uint64_t targetBytes);

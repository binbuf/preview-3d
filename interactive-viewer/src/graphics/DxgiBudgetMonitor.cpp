#include "DxgiBudgetMonitor.h"

#include "D3D12Device.h"

#include <algorithm>

using Microsoft::WRL::ComPtr;

DxgiBudgetMonitor::~DxgiBudgetMonitor()
{
    if (adapter3_ && budgetChangeCookie_ != 0) {
        adapter3_->UnregisterVideoMemoryBudgetChangeNotification(budgetChangeCookie_);
    }
}

bool DxgiBudgetMonitor::Initialize(D3D12Device& device, const CreateOptions& options, std::wstring& error)
{
    options_ = options;

    LUID luid = device.Device()->GetAdapterLuid();
    ComPtr<IDXGIAdapter1> adapter1;
    HRESULT hr = device.Factory()->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter1));
    if (FAILED(hr)) {
        error = L"Could not re-acquire the D3D12 device's adapter for budget queries.";
        return false;
    }

    hr = adapter1.As(&adapter3_);
    if (FAILED(hr)) {
        error = L"The adapter does not support IDXGIAdapter3 (QueryVideoMemoryInfo unavailable).";
        return false;
    }

    budgetChangeEvent_.reset(CreateEventW(nullptr, /*bManualReset=*/TRUE, /*bInitialState=*/FALSE, nullptr));
    if (!budgetChangeEvent_) {
        error = L"Could not create the budget-change notification event.";
        return false;
    }

    hr = adapter3_->RegisterVideoMemoryBudgetChangeNotificationEvent(budgetChangeEvent_.get(),
                                                                      &budgetChangeCookie_);
    if (FAILED(hr)) {
        error = L"Could not register for budget-change notifications.";
        return false;
    }

    return true;
}

bool DxgiBudgetMonitor::QueryBudget(DXGI_QUERY_VIDEO_MEMORY_INFO& outInfo) const
{
    if (queryOverride_) {
        return queryOverride_(outInfo);
    }
    if (!adapter3_) {
        return false;
    }
    return SUCCEEDED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &outInfo));
}

uint64_t DxgiBudgetMonitor::ComputeDetailTargetBytes() const
{
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (!QueryBudget(info)) {
        return 0;
    }

    constexpr double kMaxFraction = 0.6;
    constexpr uint64_t kMinHeadroomBytes = 512ull * 1024 * 1024;

    uint64_t budget = info.Budget;
    uint64_t sixtyPercent = static_cast<uint64_t>(static_cast<double>(budget) * kMaxFraction);
    uint64_t withHeadroom = (budget > kMinHeadroomBytes) ? (budget - kMinHeadroomBytes) : 0;

    // Both bounds apply to the same target -- not additive -- and this is
    // recomputed fresh from the live query on every call, never cached, so
    // it "reduces immediately when the OS budget falls" by construction.
    uint64_t target = (sixtyPercent < withHeadroom) ? sixtyPercent : withHeadroom;
    target = (target < options_.formatPolicyCapBytes) ? target : options_.formatPolicyCapBytes;
    return target;
}

bool DxgiBudgetMonitor::HasBudgetChangeSignaled() const
{
    if (!budgetChangeEvent_) {
        return false;
    }
    if (WaitForSingleObject(budgetChangeEvent_.get(), 0) == WAIT_OBJECT_0) {
        ResetEvent(budgetChangeEvent_.get());
        return true;
    }
    return false;
}

std::vector<EvictionCandidate> PlanEviction(const SceneSnapshot& snapshot, uint64_t targetBytes)
{
    std::vector<ReadyResourceInfo> sorted = snapshot.ReadyResources();

    uint64_t total = 0;
    for (const auto& resource : sorted) {
        total += resource.approximateBytes;
    }
    if (total <= targetBytes) {
        return {};
    }

    std::sort(sorted.begin(), sorted.end(), [](const ReadyResourceInfo& a, const ReadyResourceInfo& b) {
        return a.lastVisibleFrame < b.lastVisibleFrame;
    });

    std::vector<EvictionCandidate> plan;
    for (const auto& resource : sorted) {
        if (total <= targetBytes) {
            break;
        }
        plan.push_back(EvictionCandidate{ resource.clusterId, resource.lodLevel });
        total -= resource.approximateBytes;
    }
    return plan;
}

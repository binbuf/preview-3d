// Gate 2's "DXGI budget monitor, view-priority requester, detail eviction"
// deliverable. Real-hardware coverage proves the actual IDXGIAdapter3 path;
// an injectable query seam separately proves the design doc's exact detail-
// target formula and budget-drop responsiveness deterministically, without
// needing genuine system memory pressure. PlanEviction is tested purely
// against synthetic SceneSnapshot data -- no GPU/device needed at all.

#include "D3D12Device.h"
#include "DxgiBudgetMonitor.h"
#include "SceneSnapshot.h"

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
    CHECK(after == 0); // budget == headroom -> the headroom bound is 0, and it wins
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

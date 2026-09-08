// Gate 2 workstream B, slice 1: headless D3D12 device/queue/fence
// foundation. Proves D3D12 device/adapter mechanics via automated tests
// before any swap chain, window, shaders, or Renderer.cpp changes -- same
// discipline as proving the AppContainer sandbox before any real parser
// touched it. See .docs/design/04-rendering-and-streaming.md ("Device and
// presentation").

#include "D3D12Device.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("D3D12Device initializes against real hardware", "[graphics]")
{
    D3D12Device device;
    auto result = device.Initialize();

    REQUIRE(result.success);
    CHECK(result.hr == S_OK);
    CHECK_FALSE(result.isWarpAdapter);
    CHECK_FALSE(result.adapterDescription.empty());
    CHECK(device.Device() != nullptr);
    // Informational only, per .docs/design/04-rendering-and-streaming.md --
    // the D3D12/DXGI debug layers require the "Graphics Tools" optional
    // Windows feature, which may not be installed on every dev machine.
    // Never hard-require it.
    INFO("debug layer enabled: " << result.debugLayerEnabled);
}

TEST_CASE("D3D12Device with forceWarp always succeeds regardless of installed hardware",
          "[graphics]")
{
    D3D12Device device;
    D3D12Device::CreateOptions options;
    options.forceWarp = true;
    auto result = device.Initialize(options);

    REQUIRE(result.success);
    CHECK(result.isWarpAdapter);
    CHECK(device.Device() != nullptr);
}

TEST_CASE("Two independent D3D12Device instances can each initialize in the same process",
          "[graphics]")
{
    D3D12Device first;
    D3D12Device second;

    auto firstResult = first.Initialize();
    auto secondResult = second.Initialize();

    REQUIRE(firstResult.success);
    REQUIRE(secondResult.success);
    CHECK(first.Device() != nullptr);
    CHECK(second.Device() != nullptr);
}

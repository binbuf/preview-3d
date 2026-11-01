// T26: bounded weighted blended order-independent transparency. Pure CPU and
// shader-source contract tests; no GPU device is required.

#include "D3D12ViewerPath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Fragment
{
    float rgb[3];
    float coverage;
    float depth;
};

struct Accumulation
{
    double rgb[3]{};
    double alpha = 0.0;
    double reveal = 1.0;
};

Accumulation Accumulate(const std::vector<Fragment>& fragments, float sceneScale)
{
    Accumulation result;
    for (const auto& fragment : fragments) {
        const double weight = D3D12ViewerPath::OitWeightForDepth(fragment.depth, sceneScale);
        for (int channel = 0; channel < 3; ++channel)
            result.rgb[channel] += double(fragment.rgb[channel]) * fragment.coverage * weight;
        result.alpha += fragment.coverage * weight;
        result.reveal *= 1.0 - fragment.coverage;
    }
    return result;
}

float ResolveChannel(const Accumulation& accumulation, int channel, int background)
{
    const double transparent = accumulation.alpha > 1e-9
        ? accumulation.rgb[channel] / accumulation.alpha : 0.0;
    const double coverage = D3D12ViewerPath::OitResolveCoverage(float(accumulation.reveal));
    return float(transparent * coverage + double(background) * (1.0 - coverage));
}

std::vector<Fragment> LayeredFragments()
{
    return {
        {{0.9f, 0.15f, 0.1f}, 0.35f, 0.35f},
        {{0.1f, 0.8f, 0.2f}, 0.45f, 0.50f},
        {{0.2f, 0.3f, 0.95f}, 0.60f, 0.65f},
        {{0.95f, 0.85f, 0.1f}, 0.25f, 0.80f},
    };
}

} // namespace

TEST_CASE("Weighted blended OIT accumulation is invariant to fragment order",
          "[layered-transparency]")
{
    auto fragments = LayeredFragments();
    const auto forward = Accumulate(fragments, 2.0f);
    std::reverse(fragments.begin(), fragments.end());
    const auto reverse = Accumulate(fragments, 2.0f);

    for (int channel = 0; channel < 3; ++channel)
        CHECK(forward.rgb[channel] == Catch::Approx(reverse.rgb[channel]).margin(1e-6));
    CHECK(forward.alpha == Catch::Approx(reverse.alpha).margin(1e-6));
    CHECK(forward.reveal == Catch::Approx(reverse.reveal).margin(1e-6));

    // Every order permutation resolves to the same linear HDR colour.
    const auto shuffled = Accumulate({fragments[1], fragments[3], fragments[0], fragments[2]}, 2.0f);
    for (int channel = 0; channel < 3; ++channel)
        CHECK(ResolveChannel(forward, channel, 10) ==
              Catch::Approx(ResolveChannel(shuffled, channel, 10)).margin(1e-6f));
}

TEST_CASE("Revealage is the coverage product and coverage saturates",
          "[layered-transparency]")
{
    std::vector<Fragment> fragments;
    for (int index = 0; index < 8; ++index)
        fragments.push_back({{0.5f, 0.5f, 0.5f}, 0.2f, 0.1f + index * 0.1f});
    const auto accumulation = Accumulate(fragments, 1.0f);
    CHECK(accumulation.reveal == Catch::Approx(std::pow(0.8, 8.0)).margin(1e-9));
    CHECK(D3D12ViewerPath::OitResolveCoverage(float(accumulation.reveal)) ==
          Catch::Approx(float(1.0 - std::pow(0.8, 8.0))).margin(1e-6f));

    CHECK(D3D12ViewerPath::OitResolveCoverage(-1.0f) == 1.0f);
    CHECK(D3D12ViewerPath::OitResolveCoverage(2.0f) == 0.0f);
    CHECK(D3D12ViewerPath::OitResolveAlpha(0.0f) == 0.0f);
    CHECK(D3D12ViewerPath::kOitSupportedLayerLimit == 8u);
}

TEST_CASE("OIT weight is finite, relative to scene depth and favours near fragments",
          "[layered-transparency]")
{
    for (float depth = 0.0f; depth <= 100.0f; depth += 0.25f) {
        const float weight = D3D12ViewerPath::OitWeightForDepth(depth, 5.0f);
        CHECK(std::isfinite(weight));
        CHECK(weight >= 1e-2f);
        CHECK(weight <= 300.0f);
    }
    CHECK(D3D12ViewerPath::OitWeightForDepth(1.0f, 5.0f) >
          D3D12ViewerPath::OitWeightForDepth(4.0f, 5.0f));
    CHECK(D3D12ViewerPath::OitWeightForDepth(10.0f, 5.0f) ==
          D3D12ViewerPath::OitWeightForDepth(5.0f, 2.5f));
}

TEST_CASE("OIT target allocation is bounded by the viewport",
          "[layered-transparency]")
{
    const uint64_t smallBytes = D3D12ViewerPath::OitTargetBytesForExtent(1280, 900);
    const uint64_t largeBytes = D3D12ViewerPath::OitTargetBytesForExtent(2560, 1800);
    CHECK(smallBytes > 0);
    CHECK(largeBytes > smallBytes);
    // Two 8-byte-per-pixel targets, rounded up to 64 KiB each.
    CHECK((smallBytes <= ((uint64_t(1280) * 900 * 8 + 65535) / 65536 * 65536) * 2));
    CHECK(D3D12ViewerPath::OitTargetBytesForExtent(0, 0) == 0);
}

TEST_CASE("OIT and resolve shaders expose the bounded transparency contract",
          "[layered-transparency]")
{
    const char* pixelSource = D3D12ViewerPath::TexturedPixelShaderSource();
    const char* resolveSource = D3D12ViewerPath::ResolvePixelShaderSource();
    REQUIRE(pixelSource != nullptr);
    REQUIRE(resolveSource != nullptr);
    const std::string pixel(pixelSource);
    const std::string resolve(resolveSource);
    CHECK(pixel.find("OIT_PASS") != std::string::npos);
    CHECK(pixel.find("OitWeight") != std::string::npos);
    CHECK(pixel.find("coverage*weight") != std::string::npos);
    CHECK(resolve.find("gOitAccum") != std::string::npos);
    CHECK(resolve.find("gOitReveal") != std::string::npos);
    CHECK(resolve.find("gBackground.w>0.5f") != std::string::npos);

    ComPtr<ID3DBlob> compiled;
    ComPtr<ID3DBlob> errors;
    D3D_SHADER_MACRO defines[] = {{"OIT_PASS", "1"}, {nullptr, nullptr}};
    const HRESULT hr = D3DCompile(pixelSource, std::strlen(pixelSource), nullptr, defines,
                                  nullptr, "PSMain", "ps_5_1",
                                  D3DCOMPILE_ENABLE_STRICTNESS, 0, &compiled, &errors);
    INFO((errors ? static_cast<const char*>(errors->GetBufferPointer()) : ""));
    CHECK(SUCCEEDED(hr));
    CHECK(compiled.Get() != nullptr);

    ComPtr<ID3DBlob> resolveBlob;
    ComPtr<ID3DBlob> resolveErrors;
    const HRESULT resolveHr = D3DCompile(resolveSource, std::strlen(resolveSource), nullptr,
                                         nullptr, nullptr, "PSMain", "ps_5_1",
                                         D3DCOMPILE_ENABLE_STRICTNESS, 0, &resolveBlob,
                                         &resolveErrors);
    INFO((resolveErrors ? static_cast<const char*>(resolveErrors->GetBufferPointer()) : ""));
    CHECK(SUCCEEDED(resolveHr));
    CHECK(resolveBlob.Get() != nullptr);
}

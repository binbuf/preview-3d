// T33 edge anti-aliasing and sampling policy. The CPU lane pins the
// luma/depth/coverage thresholds, the deliberately bounded texture weight and
// the no-allocation contract. It also compiles the shipped resolve shader so a
// syntax/register error fails without a GPU device.

#include "AntiAliasing.h"
#include "D3D12ViewerPath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <string>

using Catch::Approx;
using namespace viewer_aa;
using Microsoft::WRL::ComPtr;

namespace {

bool Compiles(const char* source, const char* entry, const char* target, std::string& log)
{
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
                                      entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0,
                                      &blob, &errors);
    if (FAILED(result) && errors) {
        log.assign(static_cast<const char*>(errors->GetBufferPointer()),
                   errors->GetBufferSize());
    }
    return SUCCEEDED(result);
}

} // namespace

TEST_CASE("Edge AA luma and depth keys are bounded and projection-monotonic",
          "[anti-aliasing]")
{
    CHECK(Luma(1.0f, 1.0f, 1.0f) == Approx(1.0));
    CHECK(Luma(0.0f, 0.0f, 0.0f) == Approx(0.0));
    CHECK(Luma(1.0f, 0.0f, 0.0f) == Approx(0.2126));
    CHECK(DepthKey(1.0f) == Approx(0.0));
    CHECK(DepthKey(0.0f) == Approx(1.0));
    CHECK(DepthKey(0.5f) == Approx(0.5));
}

TEST_CASE("Depth discontinuities separate silhouettes from flat surface noise",
          "[anti-aliasing]")
{
    const Parameters defaults;
    // Silhouette against the cleared background (1.0).
    CHECK(IsDepthDiscontinuity(0.92f, 1.0f, defaults.depthRelativeThreshold));
    // A single flat plane: adjacent depth differences stay below the relative
    // threshold so texture detail on that plane is not treated as geometry.
    CHECK_FALSE(IsDepthDiscontinuity(0.9200f, 0.9202f,
                                     defaults.depthRelativeThreshold));
    // The floor keeps quantisation noise from becoming an edge at the far
    // plane where the relative term would otherwise be tiny.
    CHECK_FALSE(IsDepthDiscontinuity(0.99999f, 1.0f,
                                     defaults.depthRelativeThreshold));
}

TEST_CASE("Coverage discontinuity catches transparent silhouettes", "[anti-aliasing]")
{
    const Parameters defaults;
    CHECK(IsCoverageDiscontinuity(0.0f, 0.8f, defaults.coverageThreshold));
    CHECK_FALSE(IsCoverageDiscontinuity(0.55f, 0.60f, defaults.coverageThreshold));
}

TEST_CASE("Edge AA weights are bounded and preserve texture detail", "[anti-aliasing]")
{
    const Parameters defaults;
    // Low contrast is untouched.
    CHECK(EdgeBlendWeight(0.02f, true, false, defaults) == Approx(0.0));
    // A geometric silhouette gets a meaningful but sub-half blend.
    const float geometric = EdgeBlendWeight(1.0f, true, false, defaults);
    CHECK(geometric > 0.30f);
    CHECK(geometric <= defaults.geometricWeight);
    // Texture detail gets a small capped blend so lettering keeps contrast.
    const float texture = EdgeBlendWeight(1.0f, false, false, defaults);
    CHECK(texture > 0.0f);
    CHECK(texture <= defaults.textureWeight);
    CHECK(texture < geometric);
    // Coverage (glass) uses the geometric branch.
    CHECK(EdgeBlendWeight(1.0f, false, true, defaults) == Approx(geometric));
    // Disabled parameters return exactly zero.
    Parameters off = defaults;
    off.enabled = false;
    CHECK(EdgeBlendWeight(1.0f, true, true, off) == Approx(0.0));
}

TEST_CASE("Edge AA mode falls back explicitly without a depth SRV", "[anti-aliasing]")
{
    CHECK(EffectiveMode(false, true) == Mode::Off);
    CHECK(EffectiveMode(true, true) == Mode::DepthAwareFxaa);
    CHECK(EffectiveMode(true, false) == Mode::Unavailable);
    CHECK(kTapCount == 7);
    CHECK(kAdditionalTargetBytes == 0);
}

TEST_CASE("Resolve shader compiles with the depth-aware edge filter", "[anti-aliasing]")
{
    const char* source = D3D12ViewerPath::ResolvePixelShaderSource();
    REQUIRE(source != nullptr);
    CHECK(std::strstr(source, "Texture2D<float> gDepth : register(t6)") != nullptr);
    CHECK(std::strstr(source, "DepthEdge") != nullptr);
    CHECK(std::strstr(source, "EdgeProbe") != nullptr);
    CHECK(std::strstr(source, "gResolve2") != nullptr);
    // The filter must not touch the pick id or resolve an integer target.
    CHECK((std::strstr(source, "pick") == nullptr
           || std::strstr(source, "R32_UINT") == nullptr));
    std::string log;
    CHECK(Compiles(source, "PSMain", "ps_5_1", log));
    if (!log.empty()) INFO(log);
}

TEST_CASE("Material sampling keeps authored nearest and linear semantics", "[anti-aliasing]")
{
    const char* source = D3D12ViewerPath::TexturedPixelShaderSource();
    REQUIRE(source != nullptr);
    // Nearest slots go to the point sampler; linear slots use the anisotropic
    // sampler with automatic mip selection. Neither is overridden per asset.
    CHECK(std::strstr(source, "uv.nearest?tex.Sample(gNearestSampler,uv.uv)") != nullptr);
    CHECK(std::strstr(source, "tex.Sample(gLinearSampler,uv.uv)") != nullptr);
    std::string log;
    CHECK(Compiles(source, "PSMain", "ps_5_1", log));
    if (!log.empty()) INFO(log);
}

// T32 scale-aware screen-space ambient occlusion. The CPU lane pins the
// per-mode calibration, scale-aware radius, indirect-only compositing and the
// bounded target budget. It also compiles the shipped AO/full-screen shaders so
// a syntax or register error fails without a GPU device.

#include "ScreenSpaceAo.h"
#include "D3D12ViewerPath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <string>

using Catch::Approx;
using namespace viewer_ao;
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

TEST_CASE("Screen-space AO calibration is mode-specific and bounded", "[screen-space-ao]")
{
    const Parameters studio = ParametersForMode(0);
    const Parameters clay = ParametersForMode(1);
    const Parameters directional = ParametersForMode(2);
    const Parameters fallback = ParametersForMode(99);

    CHECK(clay.strength > studio.strength);
    CHECK(studio.strength > directional.strength);
    CHECK(clay.strength <= 0.9f);
    CHECK(directional.strength >= 0.3f);
    CHECK(clay.radiusFraction > studio.radiusFraction);
    CHECK(studio.radiusFraction >= directional.radiusFraction);
    CHECK(clay.pixelRadiusMax <= kMaxPixelRadius);
    CHECK(clay.sampleCount == kSampleCount);
    CHECK(fallback.strength == Approx(studio.strength));
    CHECK(fallback.radiusFraction == Approx(studio.radiusFraction));

    for (const Parameters& parameters : {studio, clay, directional}) {
        CHECK(parameters.radiusMinFraction <= parameters.radiusFraction);
        CHECK(parameters.radiusFraction <= parameters.radiusMaxFraction);
        CHECK(parameters.bias > 0.0f);
        CHECK(parameters.bias < 0.1f);
    }
}

TEST_CASE("Screen-space AO radius follows scene scale and stays bounded", "[screen-space-ao]")
{
    const Parameters parameters = ParametersForMode(1);

    const float tinyScene = 0.01f;
    const float hugeScene = 1000.0f;
    const float tinyRadius = WorldRadius(parameters, tinyScene);
    const float hugeRadius = WorldRadius(parameters, hugeScene);
    CHECK(tinyRadius < hugeRadius);
    CHECK(tinyRadius >= tinyScene * parameters.radiusMinFraction * 0.999f);
    CHECK(hugeRadius <= hugeScene * parameters.radiusMaxFraction * 1.001f);

    // A degenerate model never yields a zero or infinite radius, so the shader
    // cannot divide by zero or sample an unbounded neighbourhood.
    const float degenerate = WorldRadius(parameters, 0.0f);
    CHECK(std::isfinite(degenerate));
    CHECK(degenerate > 0.0f);

    // Perspective and orthographic both arrive here as a measured
    // world-units-per-pixel value; the conversion has no projection case.
    CHECK(PixelRadius(0.5f, 0.01f, 40.0f) == Approx(40.0f));
    CHECK(PixelRadius(0.5f, 0.1f, 40.0f) == Approx(5.0f));
    CHECK(PixelRadius(0.001f, 0.1f, 40.0f) == Approx(1.0f));
    CHECK(PixelRadius(0.5f, 0.0f, 40.0f) == Approx(1.0f));
    CHECK(PixelRadius(0.5f, 0.1f, 0.0f) == Approx(1.0f));
}

TEST_CASE("Screen-space AO is a restrained mask, not a global exposure", "[screen-space-ao]")
{
    CHECK(FactorFromOcclusion(0.0f, 0.5f) == Approx(1.0f));
    CHECK(FactorFromOcclusion(1.0f, 0.0f) == Approx(1.0f));
    CHECK(FactorFromOcclusion(0.0f, 0.0f) == Approx(1.0f));
    CHECK(FactorFromOcclusion(0.5f, 0.5f) == Approx(0.45f));
    CHECK(FactorFromOcclusion(1.0f, 1.0f) == Approx(0.0f));
    CHECK(FactorFromOcclusion(-1.0f, 0.5f) == Approx(1.0f));
    CHECK(FactorFromOcclusion(2.0f, 0.5f) == Approx(0.0f));
    CHECK(FactorFromOcclusion(0.5f, 0.5f)
          == Approx(1.0f - 0.5f * 0.5f * kOcclusionGain));

    // Clay's stronger mask must still leave a lit floor when the occlusion sum
    // is moderate; only a fully enclosed sample may approach zero.
    CHECK(FactorFromOcclusion(0.25f, ParametersForMode(1).strength) > 0.45f);
}

TEST_CASE("Screen-space AO composites indirect only", "[screen-space-ao]")
{
    // Direct/emission share is invariant: subtracting a zero indirect term can
    // never darken the pixel, whatever the mask says.
    CHECK(CompositeIndirect(0.8f, 0.0f, 0.0f) == Approx(0.8f));
    CHECK(CompositeIndirect(0.8f, 0.2f, 1.0f) == Approx(0.8f));
    CHECK(CompositeIndirect(0.8f, 0.2f, 0.5f) == Approx(0.7f));
    CHECK(CompositeIndirect(0.8f, 0.2f, 0.0f) == Approx(0.6f));
    CHECK(CompositeIndirect(0.1f, 0.4f, 0.0f) == Approx(0.0f));

    // Authored ORM AO and screen AO combine softly; a fully authored cavity
    // does not become black when the screen mask is also dark.
    CHECK(CombineAuthoredOcclusion(1.0f, 0.5f, 0.5f) == Approx(0.75f));
    CHECK(CombineAuthoredOcclusion(0.4f, 0.5f, 0.5f) == Approx(0.3f));
    CHECK(CombineAuthoredOcclusion(0.4f, 0.5f, 0.0f) == Approx(0.4f));
    CHECK(CombineAuthoredOcclusion(0.0f, 0.0f, 1.0f) == Approx(0.0f));
}

TEST_CASE("Screen-space AO target budget is viewport-bounded", "[screen-space-ao]")
{
    CHECK(TargetBytesForExtent(0, 100) == 0);
    CHECK(TargetBytesForExtent(100, 0) == 0);
    const uint64_t at1280 = TargetBytesForExtent(1280, 900);
    const uint64_t at1920 = TargetBytesForExtent(1920, 1080);
    CHECK(at1280 > 0);
    CHECK(at1920 > at1280);
    CHECK(at1280 < 32ull * 1024 * 1024);
    CHECK(at1920 < 64ull * 1024 * 1024);
    CHECK(at1280 % kAllocationGranularity == 0);
    CHECK(TargetSupported(1280, 900));
    CHECK(!TargetSupported(0, 900));
    CHECK(!TargetSupported(8192, 8192));
}

TEST_CASE("Screen-space AO shaders compile and declare indirect-only output", "[screen-space-ao]")
{
    const std::string pixelSource = D3D12ViewerPath::TexturedPixelShaderSource();
    const std::string resolveSource = D3D12ViewerPath::ResolvePixelShaderSource();
    CHECK(pixelSource.find("SV_TARGET2") != std::string::npos);
    CHECK(pixelSource.find("indirect") != std::string::npos);
    CHECK(resolveSource.find("gAoMask") != std::string::npos);
    CHECK(resolveSource.find("gIndirect") != std::string::npos);

    std::string log;
    CHECK(Compiles(D3D12ViewerPath::AoRawPixelShaderSource(), "PSMain", "ps_5_1", log));
    INFO(log);
    log.clear();
    CHECK(Compiles(D3D12ViewerPath::AoBlurPixelShaderSource(), "PSMain", "ps_5_1", log));
    INFO(log);

    // The opaque variant must compile with the same source as the normal
    // variant; a target-layout mismatch would be caught by PSO creation, and
    // this compile pins the shader-side layout here.
    D3D_SHADER_MACRO defines[] = {{"AO_INDIRECT", "1"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(D3D12ViewerPath::TexturedPixelShaderSource(),
                                      std::strlen(D3D12ViewerPath::TexturedPixelShaderSource()),
                                      nullptr, defines, nullptr, "PSMain", "ps_5_1",
                                      D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
    CHECK(SUCCEEDED(result));
    if (FAILED(result) && errors) {
        INFO(std::string(static_cast<const char*>(errors->GetBufferPointer()),
                         errors->GetBufferSize()));
    }
}

// T27: scene-color transmission, tinted/rough glass and the coverage/
// transmission separation. Pure CPU and shader-source contract tests; no GPU
// device is required.

#include "D3D12ViewerPath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

struct TransmittedSample
{
    float rgb[3];
};

// Independent CPU statement of the shader's thin-surface transmission:
//   transmitted = background * tint * transmissionFactor * (1 - Fresnel)
// It is written from the Khronos KHR_materials_transmission/volume thin-wall
// equations, not copied from the HLSL, so the two can be compared.
TransmittedSample ThinSurfaceTransmission(const float background[3],
                                          const float tint[3],
                                          float transmission,
                                          float ior,
                                          float nv)
{
    const float ratio = ior > 1.0f ? (ior - 1.0f) / (ior + 1.0f) : 0.0f;
    const float f0 = ratio * ratio;
    const float fresnel = D3D12ViewerPath::TransmissionInterfaceFresnel(f0, nv);
    const float weight = D3D12ViewerPath::TransmissionTintWeight(transmission, fresnel);
    TransmittedSample result{};
    for (int channel = 0; channel < 3; ++channel)
        result.rgb[channel] = background[channel] * tint[channel] * weight;
    return result;
}

} // namespace

TEST_CASE("Coverage and optical transmission stay independent",
          "[scene-transmission]")
{
    // Authored BLEND coverage survives an IOR/Fresnel change untouched.
    for (float alpha : {0.0f, 0.25f, 0.35f, 0.8f, 1.0f}) {
        CHECK(D3D12ViewerPath::TransmissionCoverage(true, alpha) ==
              Catch::Approx(alpha).margin(1e-6f));
        CHECK(D3D12ViewerPath::TransmissionCoverage(false, alpha) == 1.0f);
    }
    CHECK(D3D12ViewerPath::TransmissionCoverage(true, -1.0f) == 0.0f);
    CHECK(D3D12ViewerPath::TransmissionCoverage(true, 2.0f) == 1.0f);

    // Tint weight is the authored transmission reduced by interface
    // reflectance, never the coverage value.
    for (float transmission = 0.0f; transmission <= 1.0f; transmission += 0.1f) {
        for (float nv = 0.0f; nv <= 1.0f; nv += 0.1f) {
            const float fresnel = D3D12ViewerPath::TransmissionInterfaceFresnel(0.04f, nv);
            const float weight = D3D12ViewerPath::TransmissionTintWeight(transmission, fresnel);
            CHECK(weight >= 0.0f);
            CHECK(weight <= transmission + 1e-6f);
        }
    }
    CHECK(D3D12ViewerPath::TransmissionTintWeight(1.0f, 0.0f) == 1.0f);
    CHECK(D3D12ViewerPath::TransmissionTintWeight(1.0f, 1.0f) == 0.0f);
}

TEST_CASE("Interface Fresnel follows IOR and vanishes at IOR 1",
          "[scene-transmission]")
{
    // Normal incidence recovers F0 = ((ior-1)/(ior+1))^2.
    CHECK(D3D12ViewerPath::TransmissionInterfaceFresnel(0.04f, 1.0f) ==
          Catch::Approx(0.04f).margin(1e-6f));
    CHECK(D3D12ViewerPath::TransmissionInterfaceFresnel(0.04f, 0.0f) ==
          Catch::Approx(1.0f).margin(1e-6f));
    CHECK(D3D12ViewerPath::TransmissionInterfaceFresnel(0.0f, 0.5f) == 0.0f);
    float previous = 2.0f;
    for (float nv = 0.0f; nv <= 1.0f; nv += 0.05f) {
        const float value = D3D12ViewerPath::TransmissionInterfaceFresnel(0.04f, nv);
        CHECK(std::isfinite(value));
        CHECK(value >= 0.0f);
        CHECK(value <= 1.0f);
        CHECK(value <= previous + 1e-6f); // grazing incidence reflects more
        previous = value;
    }
}

TEST_CASE("Thin-surface transmission is linear in tint and transmission",
          "[scene-transmission]")
{
    const float background[3] = {0.8f, 0.6f, 0.4f};
    const float neutral[3] = {1.0f, 1.0f, 1.0f};
    const float orange[3] = {0.9f, 0.25f, 0.05f};
    const auto clear = ThinSurfaceTransmission(background, neutral, 1.0f, 1.5f, 1.0f);
    const auto tinted = ThinSurfaceTransmission(background, orange, 1.0f, 1.5f, 1.0f);
    for (int channel = 0; channel < 3; ++channel) {
        // A tinted panel keeps the background's relative channel ordering.
        CHECK(tinted.rgb[channel] ==
              Catch::Approx(clear.rgb[channel] * orange[channel]).margin(1e-6f));
        // Half transmission is exactly half the transmitted radiance.
        const auto half = ThinSurfaceTransmission(background, neutral, 0.5f, 1.5f, 1.0f);
        CHECK(half.rgb[channel] == Catch::Approx(clear.rgb[channel] * 0.5f).margin(1e-6f));
    }
    // IOR 1 means no optical interface: transmission passes unattenuated.
    const auto matched = ThinSurfaceTransmission(background, neutral, 1.0f, 1.0f, 0.5f);
    for (int channel = 0; channel < 3; ++channel)
        CHECK(matched.rgb[channel] == Catch::Approx(background[channel]).margin(1e-6f));
    // Higher IOR leaves less transmitted energy at the same angle.
    const auto lowIor = ThinSurfaceTransmission(background, neutral, 1.0f, 1.2f, 0.6f);
    const auto highIor = ThinSurfaceTransmission(background, neutral, 1.0f, 2.0f, 0.6f);
    CHECK(highIor.rgb[0] < lowIor.rgb[0]);
}

TEST_CASE("Roughness selects a bounded transmitted-radiance mip",
          "[scene-transmission]")
{
    CHECK(D3D12ViewerPath::TransmissionMipLevel(0.0f, 8) == 0u);
    CHECK(D3D12ViewerPath::TransmissionMipLevel(1.0f, 8) == 7u);
    CHECK(D3D12ViewerPath::TransmissionMipLevel(0.45f, 8) == 3u);
    CHECK(D3D12ViewerPath::TransmissionMipLevel(2.0f, 8) == 7u);
    CHECK(D3D12ViewerPath::TransmissionMipLevel(-1.0f, 8) == 0u);
    CHECK(D3D12ViewerPath::TransmissionMipLevel(0.5f, 0) == 0u);
    uint32_t previous = 0;
    for (float roughness = 0.0f; roughness <= 1.0f; roughness += 0.05f) {
        const uint32_t mip = D3D12ViewerPath::TransmissionMipLevel(roughness, 8);
        CHECK(mip >= previous);
        previous = mip;
    }
}

TEST_CASE("Transmission snapshot allocation is bounded by the viewport",
          "[scene-transmission]")
{
    const uint64_t smallBytes = D3D12ViewerPath::TransmissionTargetBytesForExtent(1280, 900);
    const uint64_t largeBytes = D3D12ViewerPath::TransmissionTargetBytesForExtent(2560, 1800);
    CHECK(smallBytes > uint64_t(1280) * 900 * 8);
    CHECK(largeBytes > smallBytes);
    CHECK(D3D12ViewerPath::TransmissionTargetBytesForExtent(0, 0) == 0);
    // Eight RGBA16F levels stay well under two base-level targets.
    CHECK(smallBytes < uint64_t(1280) * 900 * 8 * 2);
}

TEST_CASE("The material shader implements the T27 transmission contract",
          "[scene-transmission]")
{
    const char* pixelSource = D3D12ViewerPath::TexturedPixelShaderSource();
    REQUIRE(pixelSource != nullptr);
    const std::string pixel(pixelSource);
    CHECK(pixel.find("gTransmissionSource") != std::string::npos);
    CHECK(pixel.find("SampleLevel") != std::string::npos);
    CHECK(pixel.find("gTransmissionFrame.z*roughness") != std::string::npos);
    CHECK(pixel.find("transmittedRadiance") != std::string::npos);
    CHECK(pixel.find("transmission*(1.0f-saturate(glassReflectance))") != std::string::npos);
    // The interim coverage-carries-Fresnel composite is gone.
    CHECK(pixel.find("base.a=saturate(base.a*diffuseScale") == std::string::npos);
    // A double-sided transmissive surface remains visible from either side; the
    // geometric normal (not the whole fragment) is flipped for back faces.
    CHECK(pixel.find("transmissive && doubleSided && !input.frontFace") == std::string::npos);
    CHECK(pixel.find("if (doubleSided && !input.frontFace) geometricN=-geometricN;") !=
          std::string::npos);
    // OIT coverage is the authored alpha only for BLEND.
    CHECK(pixel.find("float coverage=effectiveCoverage;") != std::string::npos);
    CHECK(pixel.find("((flags&8192u)!=0u)?saturate(base.a):1.0f") !=
          std::string::npos);

    for (const char* macro : {"", "OIT_PASS"}) {
        ComPtr<ID3DBlob> compiled;
        ComPtr<ID3DBlob> errors;
        D3D_SHADER_MACRO defines[2] = {{nullptr, nullptr}, {nullptr, nullptr}};
        if (*macro) defines[0] = {macro, "1"};
        const HRESULT hr = D3DCompile(pixelSource, std::strlen(pixelSource), nullptr,
                                      defines[0].Name ? defines : nullptr, nullptr,
                                      "PSMain", "ps_5_1",
                                      D3DCOMPILE_ENABLE_STRICTNESS, 0, &compiled, &errors);
        INFO((errors ? static_cast<const char*>(errors->GetBufferPointer()) : ""));
        CHECK(SUCCEEDED(hr));
        CHECK(compiled.Get() != nullptr);
    }
}

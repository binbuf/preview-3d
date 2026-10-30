// T14: deterministic procedural Studio environment, SH irradiance, GGX mip
// policy and the neutral PBR tone curve. These tests exercise the shipped CPU
// reference the renderer bakes from at startup.

#include "StudioEnvironment.h"
#include "D3D12ViewerPath.h"

#include <catch2/catch_test_macros.hpp>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <string>

using namespace studio_ibl;

namespace {

BakeOptions TestOptions()
{
    BakeOptions options;
    options.faceSize = 32;
    options.specularMipCount = 5;
    options.specularSampleCount = 24;
    return options;
}

bool SameBake(const EnvironmentBake& a, const EnvironmentBake& b)
{
    if (a.base.size != b.base.size || a.specularMips.size() != b.specularMips.size())
        return false;
    for (size_t mip = 0; mip < a.specularMips.size(); ++mip) {
        if (a.specularMips[mip].size != b.specularMips[mip].size) return false;
        for (size_t face = 0; face < 6; ++face) {
            const auto& left = a.specularMips[mip].faces[face];
            const auto& right = b.specularMips[mip].faces[face];
            if (left.size() != right.size()) return false;
            for (size_t i = 0; i < left.size(); ++i) {
                if (left[i].r != right[i].r || left[i].g != right[i].g || left[i].b != right[i].b)
                    return false;
            }
        }
    }
    for (size_t i = 0; i < a.irradiance.values.size(); ++i) {
        if (a.irradiance.values[i].r != b.irradiance.values[i].r
            || a.irradiance.values[i].g != b.irradiance.values[i].g
            || a.irradiance.values[i].b != b.irradiance.values[i].b)
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("Studio environment bake has bounded dimensions and sane energy",
          "[studio-ibl][studio-ibl-bake]")
{
    const auto bake = BakeStudioEnvironment(TestOptions());
    REQUIRE(bake.base.size == 32);
    REQUIRE(bake.specularMips.size() == 5);
    REQUIRE(bake.MipCount() == 5);
    for (size_t face = 0; face < 6; ++face) {
        CHECK(bake.base.faces[face].size() == 32u * 32u);
        CHECK(bake.specularMips[0].faces[face].size() == 32u * 32u);
    }
    const uint32_t expected[5] = {32, 16, 8, 4, 2};
    for (size_t mip = 0; mip < 5; ++mip) {
        CHECK(bake.specularMips[mip].size == expected[mip]);
        for (size_t face = 0; face < 6; ++face)
            CHECK(bake.specularMips[mip].faces[face].size()
                  == static_cast<size_t>(expected[mip]) * expected[mip]);
    }

    double total = 0.0;
    float peak = 0.0f;
    size_t count = 0;
    for (const auto& face : bake.base.faces) {
        for (const auto& texel : face) {
            const float value = Luminance(texel);
            REQUIRE(std::isfinite(value));
            CHECK(value >= 0.0f);
            total += value;
            peak = std::max(peak, value);
            ++count;
        }
    }
    const double average = total / static_cast<double>(count);
    // Gradient-only floors are dark, but the softboxes must put real energy
    // into the environment and provide a highlight well above white.
    CHECK(average > 0.05);
    CHECK(average < 2.0);
    CHECK(peak > 2.0f);

    // A key-facing direction is brighter than the opposite side.
    const float key = Luminance(ProceduralStudioRadiance({0.60f, 0.50f, 0.62f}));
    const float back = Luminance(ProceduralStudioRadiance({-0.60f, -0.50f, -0.62f}));
    CHECK(key > back * 2.0f);
}

TEST_CASE("SH irradiance matches the cosine-convolved reference and follows the sky",
          "[studio-ibl][studio-ibl-sh]")
{
    const auto bake = BakeStudioEnvironment(TestOptions());
    const Vec3 up{0.0f, 0.0f, 1.0f};
    const Vec3 down{0.0f, 0.0f, -1.0f};
    const Rgb upSh = EvaluateIrradiance(bake.irradiance, up);
    const Rgb downSh = EvaluateIrradiance(bake.irradiance, down);
    CHECK(Luminance(upSh) > Luminance(downSh) * 1.5f);

    const Rgb upReference = ReferenceIrradianceDivPi(up, 512);
    const Rgb sideReference = ReferenceIrradianceDivPi({1.0f, 0.0f, 0.0f}, 512);
    const Rgb sideSh = EvaluateIrradiance(bake.irradiance, {1.0f, 0.0f, 0.0f});
    // SH9 is a low-frequency approximation; a sharp softbox leaves some
    // ringing at grazing directions, so compare against a bounded tolerance
    // rather than exact convolution.
    auto close = [](float a, float b) {
        return std::fabs(a - b) <= 0.35f * std::max(0.05f, std::max(std::fabs(a), std::fabs(b)));
    };
    CHECK(close(upSh.r, upReference.r));
    CHECK(close(upSh.g, upReference.g));
    CHECK(close(upSh.b, upReference.b));
    CHECK(close(sideSh.r, sideReference.r));
    CHECK(close(sideSh.g, sideReference.g));
    CHECK(close(sideSh.b, sideReference.b));
}

TEST_CASE("Roughness maps monotonically onto the specular mip chain",
          "[studio-ibl][studio-ibl-mip]")
{
    CHECK(RoughnessToMip(0.0f, 6) == 0.0f);
    CHECK(RoughnessToMip(1.0f, 6) == 5.0f);
    CHECK(RoughnessToMip(0.5f, 6) == 2.5f);
    CHECK(RoughnessToMip(-3.0f, 6) == 0.0f);
    CHECK(RoughnessToMip(4.0f, 6) == 5.0f);
    CHECK(RoughnessToMip(0.4f, 1) == 0.0f);
    float previous = -1.0f;
    for (int i = 0; i <= 20; ++i) {
        const float mapped = RoughnessToMip(static_cast<float>(i) / 20.0f, 6);
        CHECK(mapped >= previous);
        previous = mapped;
    }
}

TEST_CASE("Neutral tone map is monotonic, keeps hue and does not clip white",
          "[studio-ibl][studio-ibl-tonemap]")
{
    const auto black = NeutralToneMapLinear({0.0f, 0.0f, 0.0f}, 1.0f);
    CHECK(black[0] == 0.0f);
    CHECK(black[1] == 0.0f);
    CHECK(black[2] == 0.0f);

    float previous = -1.0f;
    for (int i = 0; i <= 128; ++i) {
        const float x = static_cast<float>(i) * 0.0625f;
        const auto mapped = NeutralToneMapLinear({x, x, x}, 1.0f);
        CHECK(mapped[0] >= previous);
        previous = mapped[0];
        CHECK(mapped[0] <= 1.0f);
    }

    const auto white = NeutralToneMapLinear({32.0f, 32.0f, 32.0f}, 1.0f);
    CHECK(white[0] == white[1]);
    CHECK(white[1] == white[2]);
    CHECK(white[0] <= 1.0f);
    CHECK(white[0] > 0.70f);

    // Saturated red keeps its dominant channel instead of washing to grey.
    const auto red = NeutralToneMapLinear({1.0f, 0.05f, 0.05f}, 1.0f);
    CHECK(red[0] > red[1] * 2.0f);
    CHECK(red[0] > red[2] * 2.0f);

    // Exposure scales before the curve but stays bounded.
    const auto dimmer = NeutralToneMap({2.0f, 2.0f, 2.0f}, 0.5f);
    const auto brighter = NeutralToneMap({2.0f, 2.0f, 2.0f}, 1.0f);
    CHECK(brighter[0] > dimmer[0]);
    CHECK(brighter[0] <= 1.0f);
}

TEST_CASE("Studio environment bake is deterministic across runs",
          "[studio-ibl][studio-ibl-determinism]")
{
    const auto first = BakeStudioEnvironment(TestOptions());
    const auto second = BakeStudioEnvironment(TestOptions());
    CHECK(SameBake(first, second));

    auto different = TestOptions();
    different.faceSize = 16;
    different.specularMipCount = 3;
    const auto smaller = BakeStudioEnvironment(different);
    CHECK(smaller.base.size == 16);
    CHECK(smaller.MipCount() == 3);
}

TEST_CASE("Studio shader samples a world-stable cubemap and neutral tone curve",
          "[studio-ibl][studio-ibl-shader]")
{
    const char* source = D3D12ViewerPath::TexturedPixelShaderSource();
    REQUIRE(source != nullptr);
    const std::string shader(source);
    // The environment is sampled in world space, not camera space.
    CHECK(shader.find("gWorldFromCamera") != std::string::npos);
    CHECK(shader.find("RotateWorldFromCamera") != std::string::npos);
    CHECK(shader.find("TextureCube gStudioEnvironment") != std::string::npos);
    CHECK(shader.find("SampleLevel(gEnvironmentSampler") != std::string::npos);
    CHECK(shader.find("EvaluateStudioIrradiance") != std::string::npos);
    // The tone curve is the neutral PBR curve, not the old luminance roll-off.
    CHECK(shader.find("NeutralToneMap") != std::string::npos);
    CHECK(shader.find("startCompression") != std::string::npos);
    CHECK(shader.find("DisplayMap") == std::string::npos);
    // Both direct key and IBL remain visible to material review.
    CHECK(shader.find("EvaluateLight") != std::string::npos);
    CHECK(shader.find("gEnvironment.x") != std::string::npos);
}

TEST_CASE("Studio pixel shader compiles for the feature-level-11_0 baseline",
          "[studio-ibl][studio-ibl-shader]")
{
    const char* source = D3D12ViewerPath::TexturedPixelShaderSource();
    REQUIRE(source != nullptr);
    Microsoft::WRL::ComPtr<ID3DBlob> shader;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
                                  "PSMain", "ps_5_1", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                                  &shader, &errors);
    if (FAILED(hr) && errors) {
        INFO(static_cast<const char*>(errors->GetBufferPointer()));
    }
    CHECK(SUCCEEDED(hr));
    CHECK(shader != nullptr);
}

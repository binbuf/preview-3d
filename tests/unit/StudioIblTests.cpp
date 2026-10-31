// T14: deterministic procedural Studio environment, SH irradiance, GGX mip
// policy and the neutral PBR tone curve. These tests exercise the shipped CPU
// reference the renderer bakes from at startup.

#include "StudioEnvironment.h"
#include "D3D12ViewerPath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <functional>
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

namespace {

float DotProduct(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

CubeImage BuildCube(uint32_t size, const std::function<Rgb(const Vec3&)>& radiance)
{
    CubeImage image;
    image.size = size;
    for (uint32_t face = 0; face < 6; ++face) {
        image.faces[face].resize(static_cast<size_t>(size) * size);
        for (uint32_t y = 0; y < size; ++y)
            for (uint32_t x = 0; x < size; ++x)
                image.faces[face][static_cast<size_t>(y) * size + x] =
                    radiance(CubemapDirection(face, x, y, size));
    }
    return image;
}

std::function<Rgb(const Vec3&)> LobeAround(const Vec3& axis)
{
    return [axis](const Vec3& direction) {
        const float x = std::max(0.0f, DotProduct(direction, axis));
        const float square = x * x;
        return Rgb{0.10f + 1.10f * square, 0.08f + 0.70f * square,
                   0.06f + 0.45f * square};
    };
}

Vec3 Rotated(const Vec3& axis, float radians)
{
    const float c = std::cos(radians), s = std::sin(radians);
    return {c * axis.x - s * axis.y, s * axis.x + c * axis.y, axis.z};
}

} // namespace

TEST_CASE("Cubemap texels carry exact solid angles that sum to 4pi",
          "[studio-ibl][brdf][ibl-solid-angle]")
{
    for (uint32_t size : {4u, 8u, 32u}) {
        double total = 0.0;
        float smallest = 1e9f, largest = 0.0f;
        for (uint32_t face = 0; face < 6; ++face) {
            for (uint32_t y = 0; y < size; ++y) {
                for (uint32_t x = 0; x < size; ++x) {
                    const float omega = CubemapTexelSolidAngle(face, x, y, size);
                    CHECK(omega > 0.0f);
                    total += omega;
                    smallest = std::min(smallest, omega);
                    largest = std::max(largest, omega);
                }
            }
        }
        CHECK(total == Catch::Approx(4.0 * 3.14159265358979323846).epsilon(1e-5));
        // Corners see a much smaller solid angle than face centres; an equal
        // 4*pi/(6*size*size) weight would hide exactly this difference.
        CHECK(largest > smallest * 1.4f);
    }
    CHECK(CubemapTexelSolidAngle(6, 0, 0, 8) == 0.0f);
    CHECK(CubemapTexelSolidAngle(0, 8, 0, 8) == 0.0f);
}

TEST_CASE("Solid-angle SH projection reproduces a constant environment exactly",
          "[studio-ibl][brdf][ibl-irradiance]")
{
    const Rgb constant{0.70f, 0.50f, 0.30f};
    const auto cube = BuildCube(32, [constant](const Vec3&) { return constant; });
    const auto coefficients = ProjectCubeIrradiance(cube);
    for (const Vec3& normal : {Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{0, -1, 0},
                               Vec3{0.577f, 0.577f, 0.577f}}) {
        const Rgb value = EvaluateIrradiance(coefficients, normal);
        CHECK(value.r == Catch::Approx(constant.r).margin(2e-4f));
        CHECK(value.g == Catch::Approx(constant.g).margin(2e-4f));
        CHECK(value.b == Catch::Approx(constant.b).margin(2e-4f));
    }
}

TEST_CASE("Directional irradiance matches an independent sphere reference and rotates",
          "[studio-ibl][brdf][ibl-irradiance]")
{
    const Vec3 axis{0.35f, -0.42f, 0.84f};
    const auto radiance = LobeAround(axis);
    const auto cube = BuildCube(48, radiance);
    const auto coefficients = ProjectCubeIrradiance(cube);
    auto close = [](float actual, float reference) {
        return std::fabs(actual - reference) <= 0.12f * std::max(0.05f, reference)
            + 0.008f;
    };
    for (const Vec3& normal : {axis, Vec3{0, 0, 1}, Vec3{1, 0, 0},
                               Vec3{-axis.x, -axis.y, -axis.z}}) {
        const Rgb reference = ReferenceIrradianceDivPi(radiance, normal, 20000);
        const Rgb value = EvaluateIrradiance(coefficients, normal);
        CHECK(close(value.r, reference.r));
        CHECK(close(value.g, reference.g));
        CHECK(close(value.b, reference.b));
    }

    // Rotating the environment must rotate the response with it: the response
    // along the rotated axis in the rotated environment equals the original
    // response along the original axis (up to SH9 truncation).
    const Vec3 rotatedAxis = Rotated(axis, 1.2f);
    const auto rotatedCube = BuildCube(48, LobeAround(rotatedAxis));
    const auto rotatedCoefficients = ProjectCubeIrradiance(rotatedCube);
    const Rgb alongRotated = EvaluateIrradiance(rotatedCoefficients, rotatedAxis);
    const Rgb alongOriginal = EvaluateIrradiance(coefficients, axis);
    CHECK(Luminance(alongRotated) == Catch::Approx(Luminance(alongOriginal)).epsilon(0.08));
    const Rgb awayFromRotated = EvaluateIrradiance(rotatedCoefficients, axis);
    CHECK(Luminance(alongRotated) > Luminance(awayFromRotated) * 1.15f);
}

TEST_CASE("GGX prefilter preserves constant energy and matches an independent integral",
          "[studio-ibl][brdf][ibl-prefilter]")
{
    const Rgb constant{0.40f, 0.55f, 0.25f};
    auto constantRadiance = [constant](const Vec3&) { return constant; };
    for (float roughness : {0.0f, 0.35f, 0.7f, 1.0f}) {
        for (const Vec3& normal : {Vec3{0, 0, 1}, Vec3{0.6f, 0.0f, 0.8f},
                                   Vec3{0.0f, -0.8f, 0.6f}}) {
            const Rgb value = PrefilterSpecular(constantRadiance, normal, roughness, 256);
            CHECK(value.r == Catch::Approx(constant.r).margin(3e-3f));
            CHECK(value.g == Catch::Approx(constant.g).margin(3e-3f));
            CHECK(value.b == Catch::Approx(constant.b).margin(3e-3f));
        }
    }

    const Vec3 axis{0.2f, 0.5f, 0.84f};
    const auto radiance = LobeAround(axis);
    for (float roughness : {0.3f, 0.55f, 0.9f}) {
        const Vec3 normal = axis;
        const Rgb production = PrefilterSpecular(radiance, normal, roughness, 8192);
        const Rgb reference = ReferencePrefilteredRadiance(radiance, normal, roughness, 65536);
        auto close = [](float actual, float expected) {
            return std::fabs(actual - expected) <= 0.06f * std::max(0.05f, expected) + 0.004f;
        };
        INFO("roughness " << roughness);
        CHECK(close(production.r, reference.r));
        CHECK(close(production.g, reference.g));
        CHECK(close(production.b, reference.b));
    }

    // Rotating the environment rotates the prefiltered reflection.
    const Vec3 rotatedAxis = Rotated(axis, 0.9f);
    const Rgb rotated = PrefilterSpecular(LobeAround(rotatedAxis), rotatedAxis, 0.5f, 4096);
    const Rgb original = PrefilterSpecular(LobeAround(axis), rotatedAxis, 0.5f, 4096);
    CHECK(Luminance(rotated) > Luminance(original) * 1.15f);
}

TEST_CASE("Split-sum BRDF LUT matches an independent integration and conserves white energy",
          "[studio-ibl][brdf][ibl-lut]")
{
    const BrdfLut lut = BakeBrdfLut(64, 256);
    REQUIRE(!lut.Empty());
    CHECK(lut.size == 64);

    // The independent reference uses a uniform half-vector quadrature, which
    // resolves the lobe at mid/high roughness. Below ~0.4 the GGX peak is much
    // narrower than one quadrature cell; the production importance sampler is
    // the appropriate estimator there, and its white-furnace/monotonic checks
    // below cover it.
    for (float nv : {0.12f, 0.45f, 0.85f}) {
        for (float roughness : {0.45f, 0.75f, 0.98f}) {
            const auto actual = SampleBrdfLut(lut, nv, roughness);
            const auto reference = ReferenceBrdfIntegration(nv, roughness, 65536);
            INFO("NoV " << nv << " roughness " << roughness);
            CHECK(actual[0] >= 0.0f);
            CHECK(actual[1] >= 0.0f);
            CHECK(actual[0] <= 1.05f);
            CHECK(actual[1] <= 1.05f);
            CHECK(actual[0] == Catch::Approx(reference[0]).margin(0.03f));
            CHECK(actual[1] == Catch::Approx(reference[1]).margin(0.03f));
        }
    }

    // A white metal (F0 = 1) must reflect exactly the environment energy after
    // the multiple-scattering compensation, at every roughness.
    for (float nv : {0.05f, 0.4f, 0.9f}) {
        for (float roughness : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            const auto white = EnvironmentBrdfFresnel(lut, nv, roughness, {1, 1, 1}, 1.0f);
            INFO("NoV " << nv << " roughness " << roughness);
            CHECK(white[0] == Catch::Approx(1.0f).margin(2e-3f));
            CHECK(white[1] == Catch::Approx(1.0f).margin(2e-3f));
            CHECK(white[2] == Catch::Approx(1.0f).margin(2e-3f));
        }
    }

    // A black dielectric still gains its IOR Fresnel reflection at grazing.
    const auto blackFacing = EnvironmentBrdfFresnel(lut, 0.9f, 0.4f, {0.04f, 0.04f, 0.04f}, 1.0f);
    const auto blackGrazing = EnvironmentBrdfFresnel(lut, 0.05f, 0.4f, {0.04f, 0.04f, 0.04f}, 1.0f);
    CHECK(blackFacing[0] > 0.02f);
    CHECK(blackFacing[0] < 0.10f);
    CHECK(blackGrazing[0] > blackFacing[0] * 3.0f);
    // KHR_materials_specular factor 0 removes the dielectric reflection.
    const auto noSpecular = EnvironmentBrdfFresnel(lut, 0.9f, 0.4f, {0.04f, 0.04f, 0.04f}, 0.0f);
    CHECK(noSpecular[0] == Catch::Approx(0.0f).margin(1e-4f));
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
    // Exactly one display encoding: the material shader writes linear HDR and
    // must not contain the tone curve or the sRGB transfer. The resolve pass
    // is the only place that maps to the display.
    CHECK(shader.find("NeutralToneMap") == std::string::npos);
    CHECK(shader.find("startCompression") == std::string::npos);
    CHECK(shader.find("DisplayMap") == std::string::npos);
    const char* resolveSource = D3D12ViewerPath::ResolvePixelShaderSource();
    REQUIRE(resolveSource != nullptr);
    const std::string resolve(resolveSource);
    CHECK(resolve.find("NeutralToneMap") != std::string::npos);
    CHECK(resolve.find("startCompression") != std::string::npos);
    CHECK(resolve.find("SrgbEncode") != std::string::npos);
    // Both direct key and IBL remain visible to material review.
    CHECK(shader.find("EvaluateLightSplit") != std::string::npos);
    CHECK(shader.find("gEnvironment.x") != std::string::npos);
    // T24 split-sum environment BRDF and Fresnel-weighted IBL.
    CHECK(shader.find("EnvironmentBrdfFresnel") != std::string::npos);
    CHECK(shader.find("gBrdfLut") != std::string::npos);
    CHECK(shader.find("SampleBrdfLut") != std::string::npos);
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

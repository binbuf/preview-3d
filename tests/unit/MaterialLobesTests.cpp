// T18: protocol-v12 material lobes. These are pure CPU/source-contract tests:
// no GPU device is required, so Tests.Unit can run on every CI lane.

#include "model_core/MaterialPayload.h"
#include "model_core/WireFormat.h"
#include "D3D12ViewerPath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>

using namespace model_core;

TEST_CASE("Protocol v13 MaterialPayload is a fixed 244-byte lobe and texture record",
          "[material-lobes][wire-format]")
{
    REQUIRE(kCurrentProtocolVersion == 13);
    REQUIRE(sizeof(MaterialPayload) == 244);
    CHECK(offsetof(MaterialPayload, transmissionFactor) == 68);
    CHECK(offsetof(MaterialPayload, emissiveStrength) == 72);
    CHECK(offsetof(MaterialPayload, clearcoatFactor) == 76);
    CHECK(offsetof(MaterialPayload, clearcoatRoughness) == 80);
    CHECK(offsetof(MaterialPayload, sheenColor) == 84);
    CHECK(offsetof(MaterialPayload, sheenRoughness) == 96);
    CHECK(offsetof(MaterialPayload, specularFactor) == 100);
    CHECK(offsetof(MaterialPayload, specularColor) == 104);
    CHECK(offsetof(MaterialPayload, ior) == 116);
    CHECK(offsetof(MaterialPayload, occlusionStrength) == 120);
    CHECK(offsetof(MaterialPayload, reserved) == 228);
    CHECK(kMaterialReservedWordCount == 4);
}

TEST_CASE("Value-initialized material keeps pre-v12 defaults and zero reserved words",
          "[material-lobes]")
{
    MaterialPayload material{};
    CHECK(material.emissiveStrength == 1.0f);
    CHECK(material.clearcoatFactor == 0.0f);
    CHECK(material.clearcoatRoughness == 0.0f);
    CHECK(material.sheenColor[0] == 0.0f);
    CHECK(material.sheenColor[1] == 0.0f);
    CHECK(material.sheenColor[2] == 0.0f);
    CHECK(material.sheenRoughness == 0.0f);
    CHECK(material.specularFactor == 1.0f);
    CHECK(material.specularColor[0] == 1.0f);
    CHECK(material.specularColor[1] == 1.0f);
    CHECK(material.specularColor[2] == 1.0f);
    CHECK(material.ior == kDefaultMaterialIor);
    CHECK(material.occlusionStrength == 1.0f);
    CHECK(material.reserved[0] == 0);
    CHECK(material.reserved[3] == 0);
    CHECK(ValidateMaterialLobeFields(material) == nullptr);
}

TEST_CASE("IOR and specular fields produce the documented dielectric F0",
          "[material-lobes]")
{
    // (1.5 - 1) / (1.5 + 1) squared = 0.04, matching the pre-T18 constant.
    CHECK(IorToF0(1.5f) == Catch::Approx(0.04f).margin(1e-6f));
    CHECK(IorToF0(1.0f) == 0.0f);
    CHECK(IorToF0(0.5f) == 0.0f);
    CHECK(IorToF0(1.33f) == Catch::Approx(0.02005f).margin(1e-4f));

    MaterialPayload material{};
    float f0[3]{};
    MaterialDielectricF0(material, f0);
    CHECK(f0[0] == Catch::Approx(0.04f).margin(1e-6f));

    material.ior = 1.33f;
    material.specularFactor = 0.5f;
    material.specularColor[0] = 1.0f;
    material.specularColor[1] = 0.5f;
    material.specularColor[2] = 0.25f;
    MaterialDielectricF0(material, f0);
    CHECK(f0[0] == Catch::Approx(0.010025f).margin(1e-4f));
    CHECK(f0[1] == Catch::Approx(0.0050125f).margin(1e-4f));
    CHECK(f0[2] == Catch::Approx(0.002506f).margin(1e-4f));
}

TEST_CASE("Clearcoat and sheen lobes are distinct and do not double-count base energy",
          "[material-lobes]")
{
    MaterialPayload plain{};
    CHECK(ClearcoatBaseSpecularScale(plain) == 1.0f);
    CHECK(SheenLobeWeight(plain, 0.5f) == 0.0f);

    MaterialPayload paint{};
    paint.clearcoatFactor = 1.0f;
    paint.clearcoatRoughness = 0.1f;
    CHECK(ClearcoatBaseSpecularScale(paint) == 0.0f); // coat owns the whole specular layer

    MaterialPayload cloth{};
    cloth.sheenColor[0] = cloth.sheenColor[1] = cloth.sheenColor[2] = 0.8f;
    cloth.sheenRoughness = 1.0f;
    // Grazing view: sheen is present and stronger for the rougher weave.
    const float grazing = SheenLobeWeight(cloth, 0.1f);
    const float facing = SheenLobeWeight(cloth, 0.9f);
    CHECK(grazing > facing * 10.0f);
    cloth.sheenRoughness = 0.0f;
    CHECK(grazing > SheenLobeWeight(cloth, 0.1f));

    // Emissive strength is a post-lighting multiplier, not a tint change.
    MaterialPayload sign{};
    sign.emissiveFactor[0] = 1.0f;
    sign.emissiveFactor[1] = 0.7f;
    sign.emissiveFactor[2] = 0.2f;
    sign.emissiveStrength = 5.0f;
    CHECK(sign.emissiveFactor[0] * sign.emissiveStrength == Catch::Approx(5.0f));
    CHECK(sign.emissiveFactor[1] * sign.emissiveStrength == Catch::Approx(3.5f));
}

TEST_CASE("Lobe validation rejects out-of-range and reserved non-zero words",
          "[material-lobes]")
{
    MaterialPayload material{};
    material.clearcoatFactor = 1.5f;
    CHECK(std::strcmp(ValidateMaterialLobeFields(material), "material clearcoatFactor out of range") == 0);

    material = MaterialPayload{};
    material.sheenColor[2] = -0.01f;
    CHECK(std::strcmp(ValidateMaterialLobeFields(material), "material sheenColor out of range") == 0);

    material = MaterialPayload{};
    material.specularFactor = 1.01f;
    CHECK(std::strcmp(ValidateMaterialLobeFields(material), "material specularFactor out of range") == 0);

    material = MaterialPayload{};
    material.ior = 0.99f;
    CHECK(std::strcmp(ValidateMaterialLobeFields(material), "material ior out of range") == 0);

    material = MaterialPayload{};
    material.occlusionStrength = 2.0f;
    CHECK(std::strcmp(ValidateMaterialLobeFields(material), "material occlusionStrength out of range") == 0);

    material = MaterialPayload{};
    material.reserved[2] = 1;
    CHECK(std::strcmp(ValidateMaterialLobeFields(material), "material reserved word must be zero") == 0);

    material = MaterialPayload{};
    material.emissiveStrength = std::numeric_limits<float>::quiet_NaN();
    CHECK(ValidateMaterialLobeFields(material) != nullptr);
}

TEST_CASE("T22 authored texture fields default to identity and validate their bounds",
          "[material-lobes][material-semantics]")
{
    MaterialPayload material{};
    CHECK(sizeof(MaterialPayload) == 244);
    CHECK(offsetof(MaterialPayload, normalScale) == 124);
    CHECK(offsetof(MaterialPayload, slotFlags) == 128);
    CHECK(offsetof(MaterialPayload, slotUvOffset) == 148);
    CHECK(offsetof(MaterialPayload, slotUvScale) == 180);
    CHECK(offsetof(MaterialPayload, slotUvRotation) == 212);
    CHECK(material.normalScale == 1.0f);
    for (uint32_t slot = 0; slot < kMaterialTextureSlotCount; ++slot) {
        CHECK(material.slotFlags[slot] == 0);
    }
    for (uint32_t slot = 0; slot < kMaterialTextureSlotCount - 1; ++slot) {
        CHECK(material.slotUvOffset[slot][0] == 0.0f);
        CHECK(material.slotUvScale[slot][0] == 1.0f);
        CHECK(material.slotUvScale[slot][1] == 1.0f);
        CHECK(material.slotUvRotation[slot] == 0.0f);
    }
    CHECK(kMaterialMaxSupportedUvSet == 0);

    material.normalScale = 0.45f;
    material.slotFlags[2] = MaterialSlotFlags(0, TextureAddressId::Clamp,
                                              TextureAddressId::Mirror, true);
    CHECK(MaterialSlotUvSet(material.slotFlags[2]) == 0);
    CHECK(MaterialSlotUvSetSupported(material.slotFlags[2]));
    CHECK(ValidateMaterialLobeFields(material) == nullptr);

    material = MaterialPayload{};
    material.normalScale = 17.0f;
    CHECK(std::strcmp(ValidateMaterialLobeFields(material), "material normalScale out of range") == 0);

    material = MaterialPayload{};
    material.slotFlags[1] = 0x80000000u;
    CHECK(std::strcmp(ValidateMaterialLobeFields(material), "material slot flags has unrecognized bits set") == 0);

    material = MaterialPayload{};
    material.slotFlags[1] = MaterialSlotFlags(16, TextureAddressId::Wrap, TextureAddressId::Wrap, false);
    CHECK(std::strcmp(ValidateMaterialLobeFields(material), "material texture uv set out of range") == 0);

    material = MaterialPayload{};
    material.slotUvScale[3][1] = std::numeric_limits<float>::infinity();
    CHECK(ValidateMaterialLobeFields(material) != nullptr);

    material = MaterialPayload{};
    material.slotUvRotation[0] = 100.0f;
    CHECK(std::strcmp(ValidateMaterialLobeFields(material), "material slot uv transform out of range") == 0);
}

TEST_CASE("Textured shader applies the layered lobes before the HDR resolve",
          "[material-lobes][material-lobes-shader]")
{
    const char* source = D3D12ViewerPath::TexturedPixelShaderSource();
    REQUIRE(source != nullptr);
    const std::string shader(source);
    CHECK(shader.find("gMaterialLobes0") != std::string::npos);
    CHECK(shader.find("IorToF0") != std::string::npos);
    CHECK(shader.find("EvaluateClearcoatLight") != std::string::npos);
    CHECK(shader.find("gMaterialLobes0.x") != std::string::npos); // clearcoat factor
    CHECK(shader.find("gMaterialLobes0.y") != std::string::npos); // clearcoat roughness
    CHECK(shader.find("gMaterialLobes0.z") != std::string::npos); // occlusion strength
    CHECK(shader.find("gMaterialLobes0.w") != std::string::npos); // sheen roughness
    CHECK(shader.find("gMaterialLobes1.rgb") != std::string::npos); // sheen
    CHECK(shader.find("gMaterialLobes1.w") != std::string::npos);   // specular factor
    CHECK(shader.find("gMaterialLobes2.x") != std::string::npos);   // IOR
    CHECK(shader.find("gMaterialLobes2.yzw") != std::string::npos); // specular color
    CHECK(shader.find("gEmissiveFactor.a") != std::string::npos);   // emissive strength
    CHECK(shader.find("gMaterialLobes3") == std::string::npos);     // root-constant budget
    CHECK(shader.find("(1.0f-clearcoat)") != std::string::npos);    // energy layering
    // The old fixed 0.04 dielectric F0 must not remain in EvaluateLight.
    CHECK(shader.find("float3 f0=lerp(float3(.04f,.04f,.04f),base,metallic)") == std::string::npos);

    Microsoft::WRL::ComPtr<ID3DBlob> compiled;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
                                  "PSMain", "ps_5_1", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                                  &compiled, &errors);
    if (FAILED(hr) && errors)
        INFO(static_cast<const char*>(errors->GetBufferPointer()));
    CHECK(SUCCEEDED(hr));
    CHECK(compiled != nullptr);
}

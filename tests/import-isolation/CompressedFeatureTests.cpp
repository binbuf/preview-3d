#define NOMINMAX
#include "MeshoptDecodeAdapter.h"
#include "WebpDecodeAdapter.h"

#include <catch2/catch_test_macros.hpp>
#include <meshoptimizer.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifndef PREVIEW3D_TEST_ASSETS_DIR
#error "PREVIEW3D_TEST_ASSETS_DIR must be defined by Tests.ImportIsolation.vcxproj"
#endif

using namespace import_worker;
using model_core::ImportErrorCode;

namespace {

std::vector<std::byte> ReadBytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    std::vector<char> chars{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    std::vector<std::byte> bytes(chars.size());
    std::memcpy(bytes.data(), chars.data(), chars.size());
    return bytes;
}

} // namespace

TEST_CASE("meshopt decode boundary accepts a pinned vertex stream and enforces unit limits",
          "[meshopt][compressed]")
{
    const std::array<float, 9> positions{0,0,0, 1,0,0, 0,1,0};
    std::vector<unsigned char> encoded(meshopt_encodeVertexBufferBound(3, sizeof(float) * 3));
    const size_t encodedSize = meshopt_encodeVertexBuffer(
        encoded.data(), encoded.size(), positions.data(), 3, sizeof(float) * 3);
    REQUIRE(encodedSize > 0);
    encoded.resize(encodedSize);
    const auto bytes = std::as_bytes(std::span(encoded));

    auto result = DecodeMeshoptBuffer(bytes, 3, 12, 36, MeshoptDecodeMode::Attributes,
                                      MeshoptDecodeFilter::None);
    REQUIRE(std::holds_alternative<std::vector<std::byte>>(result));
    const auto& decoded = std::get<std::vector<std::byte>>(result);
    REQUIRE(decoded.size() == sizeof(positions));
    CHECK(std::memcmp(decoded.data(), positions.data(), decoded.size()) == 0);

    MeshoptDecodeOptions limited;
    limited.maxDecodedBytes = 35;
    auto overLimit = DecodeMeshoptBuffer(bytes, 3, 12, 36, MeshoptDecodeMode::Attributes,
                                         MeshoptDecodeFilter::None, limited);
    CHECK(std::get<ImportErrorCode>(overLimit) == ImportErrorCode::ResourceLimit);
    auto malformed = DecodeMeshoptBuffer(bytes, 3, 10, 30, MeshoptDecodeMode::Attributes,
                                         MeshoptDecodeFilter::None);
    CHECK(std::get<ImportErrorCode>(malformed) == ImportErrorCode::MalformedData);
}

TEST_CASE("WebP decode boundary validates bytes and decoded budgets", "[webp][texture][compressed]")
{
    const auto path = std::filesystem::path(PREVIEW3D_TEST_ASSETS_DIR) / L"corpus" / L"sample.webp";
    const auto encoded = ReadBytes(path);
    REQUIRE_FALSE(encoded.empty());

    auto decoded = DecodeWebpImage(encoded, model_core::ColorSpaceId::Srgb);
    REQUIRE(decoded.has_value());
    CHECK(decoded->width == 1);
    CHECK(decoded->height == 1);
    CHECK(decoded->mipLevels == 1);
    REQUIRE(decoded->pixelBytes.size() == 4);
    CHECK(std::to_integer<uint8_t>(decoded->pixelBytes[3]) == 0);

    TextureDecodeOptions encodedLimit;
    encodedLimit.maxEncodedBytes = encoded.size() - 1;
    CHECK_FALSE(DecodeWebpImage(encoded, model_core::ColorSpaceId::Srgb, encodedLimit));
    TextureDecodeOptions decodedLimit;
    decodedLimit.maxDecodedBytes = 3;
    CHECK_FALSE(DecodeWebpImage(encoded, model_core::ColorSpaceId::Srgb, decodedLimit));

    auto corrupt = encoded;
    corrupt[0] = std::byte{0};
    CHECK_FALSE(DecodeWebpImage(corrupt, model_core::ColorSpaceId::Srgb));
}

// SPIKE-8b (T03) in-process coverage for the surrogate probe's throwaway
// compressed-glTF decoder (thumbnail-spike/surrogate-probe/GltfSpikeDecode.*).
// The probe test proves the decoder closure loads inside dllhost.exe; these
// cases prove the same code decodes the qualified corpus without a Shell, so a
// surrogate failure can be attributed to hosting rather than the decoder.

#include "GltfSpikeDecode.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace
{

std::wstring CorpusPath(const wchar_t* name)
{
    return (std::filesystem::path(PREVIEW3D_TEST_ASSETS_DIR) / L"corpus" / name).wstring();
}

void CheckFixture(const wchar_t* name, const char* expectedDecoder)
{
    std::vector<thumbnail_rasterizer::Triangle> triangles;
    gltf_spike::DecodeStats stats;
    std::string error;
    const bool ok = gltf_spike::Decode(CorpusPath(name), triangles, stats, error);
    CHECK(ok);
    if (!ok) {
        WARN("decode error: " << error);
        return;
    }
    CHECK_FALSE(triangles.empty());
    CHECK(stats.geometryDecoder == expectedDecoder);
    CHECK(stats.sourceBytes > 0);
}

} // namespace

TEST_CASE("SPIKE-8b decodes a Draco GLB", "[gltf-spike-decode]")
{
    CheckFixture(L"draco_triangle.glb", "draco");
}

TEST_CASE("SPIKE-8b decodes a position-only Draco GLB", "[gltf-spike-decode]")
{
    CheckFixture(L"draco_position_only.glb", "draco");
}

TEST_CASE("SPIKE-8b decodes a meshopt GLB", "[gltf-spike-decode]")
{
    CheckFixture(L"meshopt.glb", "meshopt");
}

TEST_CASE("SPIKE-8b decodes a Basis-textured GLB", "[gltf-spike-decode]")
{
    CheckFixture(L"basisu_textured_triangle.glb", "plain");
}

TEST_CASE("SPIKE-8b decodes a WebP-textured glTF", "[gltf-spike-decode]")
{
    CheckFixture(L"webp.gltf", "plain");
}

TEST_CASE("SPIKE-8b standalone image decoder handles KTX2/Basis and WebP", "[gltf-spike-decode]")
{
    gltf_spike::DecodeStats stats;
    std::string error;
    const std::filesystem::path ktx2 = std::filesystem::path(PREVIEW3D_TEST_ASSETS_DIR) / L"basisu_sample.ktx2";
    REQUIRE(gltf_spike::DecodeStandaloneImage(ktx2.wstring(), stats, error));
    CHECK(stats.decodedImages.size() == 1);
    CHECK(stats.decodedImages.front() == "ktx2/basisu");
    CHECK(stats.imagePixels > 0);

    gltf_spike::DecodeStats webpStats;
    std::string webpError;
    REQUIRE(gltf_spike::DecodeStandaloneImage(CorpusPath(L"sample.webp"), webpStats, webpError));
    CHECK(webpStats.decodedImages.size() == 1);
    CHECK(webpStats.decodedImages.front() == "webp");
    CHECK(webpStats.imagePixels > 0);
}

TEST_CASE("SPIKE-8b decoder rejects a corrupt compressed image without throwing", "[gltf-spike-decode]")
{
    std::vector<thumbnail_rasterizer::Triangle> triangles;
    gltf_spike::DecodeStats stats;
    std::string error;
    const bool ok = gltf_spike::Decode(CorpusPath(L"basisu_corrupt_ktx2.glb"), triangles, stats, error);
    CHECK(ok); // geometry is valid; only the optional KTX2 image is corrupt
    CHECK_FALSE(triangles.empty());
    CHECK(stats.decodedImages.empty());
}

TEST_CASE("SPIKE-8b decoder rejects a truncated glb", "[gltf-spike-decode]")
{
    std::vector<thumbnail_rasterizer::Triangle> triangles;
    gltf_spike::DecodeStats stats;
    std::string error;
    const bool ok = gltf_spike::Decode(CorpusPath(L"truncated.glb"), triangles, stats, error);
    CHECK_FALSE(ok);
    CHECK_FALSE(error.empty());
}
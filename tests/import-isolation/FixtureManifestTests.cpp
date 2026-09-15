#define NOMINMAX
#include "SandboxTestSupport.h"
#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/VertexLayouts.h"
#include "../../interactive-viewer/test-assets/corpus/Expectations.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>

namespace {
import_broker::ImportSessionResult Import(const wchar_t* path, import_broker::ImportFormat format)
{
    import_broker::ImportSessionRequest request;
    request.workerExePath = sandbox_test_support::WorkerExePath();
    request.sourcePath = std::wstring(PREVIEW3D_TEST_ASSETS_DIR) + path;
    request.format = format;
    request.generationId = 104;
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = import_broker::kImportMaxChunkCount;
    request.maxSidecarRequestsPerGeneration = 64;
    request.maxSidecarFileBytes = 256ull * 1024 * 1024;
    return import_broker::RunImportSession(request);
}
}

TEST_CASE("Manifest Tier A fixtures preserve counts and geometric bounds through the real sandbox", "[fixtures]")
{
    for (const auto& fixture : fixture_manifest::geometry) {
        CAPTURE(fixture.sha256);
        const std::wstring path(fixture.path);
        const auto format = path.ends_with(L".glb") ? import_broker::ImportFormat::Gltf
            : path.ends_with(L".stl") ? import_broker::ImportFormat::Stl : import_broker::ImportFormat::Ply;
        auto result = Import(fixture.path, format);
        CAPTURE(result.stage, result.errorCode);
        REQUIRE(result.ok);
        unsigned triangles = 0, points = 0, materials = 0, images = 0;
        unsigned colorMask = 0;
        std::array<float, 3> lo{ INFINITY, INFINITY, INFINITY }, hi{ -INFINITY, -INFINITY, -INFINITY };
        for (const auto& chunk : result.chunks) {
            const auto& d = chunk.descriptor;
            if (d.topology == model_core::ChunkTopology::Material) {
                ++materials;
                model_core::MaterialPayload payload{};
                REQUIRE(chunk.payload.size() == sizeof(payload));
                std::memcpy(&payload, chunk.payload.data(), sizeof(payload));
                CHECK(payload.metallicFactor == Catch::Approx(0.25f));
                CHECK(payload.roughnessFactor == Catch::Approx(0.75f));
                CHECK((payload.flags & model_core::kMaterialFlagDoubleSided) != 0);
                const std::array<std::array<float,4>,4> colors{{ {1,0,0,1}, {0,1,0,1}, {0,0,1,1}, {1,1,1,1} }};
                for (unsigned color=0; color<colors.size(); ++color) {
                    if (std::equal(colors[color].begin(),colors[color].end(),payload.baseColorFactor)) colorMask |= 1u<<color;
                }
                continue;
            }
            if (d.topology == model_core::ChunkTopology::Image) {
                ++images;
                model_core::ImagePayloadHeader header{};
                REQUIRE(chunk.payload.size() >= sizeof(header));
                std::memcpy(&header, chunk.payload.data(), sizeof(header));
                CHECK(header.width == 2);
                CHECK(header.height == 2);
                REQUIRE(header.pixelFormat == static_cast<unsigned>(model_core::PixelFormatId::RGBA8_UNORM));
                const std::array<unsigned char,16> rgba{255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};
                CHECK(header.mipLevels == 2);
                REQUIRE(header.pixelDataByteSize == rgba.size()+4);
                CHECK(std::memcmp(chunk.payload.data()+sizeof(header),rgba.data(),rgba.size()) == 0);
                continue;
            }
            if (d.topology == model_core::ChunkTopology::TriangleList) triangles += d.indexCount/3;
            else if (d.topology == model_core::ChunkTopology::PointList) points += d.vertexCount;
            else continue;
            const auto stride = model_core::VertexStrideForLayout(static_cast<model_core::VertexLayoutId>(d.vertexLayoutId));
            REQUIRE(stride >= 12);
            for (unsigned i=0; i<d.vertexCount; ++i) {
                std::array<float,3> p;
                std::memcpy(p.data(), chunk.payload.data()+i*stride,12);
                for (unsigned axis=0; axis<3; ++axis) {
                    const float world = float(d.origin[axis] + p[axis]);
                    lo[axis]=std::min(lo[axis],world); hi[axis]=std::max(hi[axis],world);
                }
            }
        }
        CHECK(triangles == fixture.triangles);
        CHECK(points == fixture.points);
        for (unsigned axis=0; axis<3; ++axis) { CHECK(lo[axis]==0); CHECK(hi[axis]==11); }
        CHECK(materials == (format == import_broker::ImportFormat::Gltf ? 4u : 0u));
        CHECK(images == (format == import_broker::ImportFormat::Gltf ? 1u : 0u));
        CHECK(colorMask == (format == import_broker::ImportFormat::Gltf ? 15u : 0u));
    }
}

TEST_CASE("Manifest hostile sidecars and over-limit headers fail while approved sidecars import", "[fixtures]")
{
    CHECK(Import(L"corpus/sidecar-approved.gltf",import_broker::ImportFormat::Gltf).ok);
    for (const auto* path : {L"corpus/sidecar-missing.gltf",L"corpus/sidecar-traversal.gltf",
             L"corpus/sidecar-encoded-traversal.gltf",L"corpus/sidecar-absolute.gltf",L"corpus/sidecar-network.gltf",
             L"corpus/sidecar-unc.gltf",L"corpus/sidecar-ads.gltf",L"corpus/truncated.glb"}) {
        CHECK_FALSE(Import(path,import_broker::ImportFormat::Gltf).ok);
    }
    CHECK_FALSE(Import(L"corpus/over-limit.stl",import_broker::ImportFormat::Stl).ok);
    CHECK_FALSE(Import(L"corpus/truncated.stl",import_broker::ImportFormat::Stl).ok);
    CHECK_FALSE(Import(L"corpus/over-limit.ply",import_broker::ImportFormat::Ply).ok);
}

#include <catch2/catch_test_macros.hpp>

#include "StepImporterVersion.h"
#include "StepTessellationProfile.h"

#include "import_broker/ImportSession.h"
#include "model_core/WireFormat.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef PREVIEW3D_STEP_HOST_EXE
#error "PREVIEW3D_STEP_HOST_EXE must be defined by Tests.ImportIsolation.vcxproj"
#endif
#ifndef PREVIEW3D_STP_FIXTURES_DIR
#error "PREVIEW3D_STP_FIXTURES_DIR must be defined by Tests.ImportIsolation.vcxproj"
#endif

namespace {

std::wstring StpFixture(const wchar_t* name)
{
    return std::wstring(PREVIEW3D_STP_FIXTURES_DIR) + name;
}

import_broker::ImportSessionRequest Request(const wchar_t* fixture, uint64_t generation,
                                            uint64_t sectionBytes, uint32_t maxBatches)
{
    import_broker::ImportSessionRequest request;
    request.format = import_broker::ImportFormat::Step;
    request.sourcePath = StpFixture(fixture);
    request.stepHostExePath = PREVIEW3D_STEP_HOST_EXE;
    request.generationId = generation;
    request.sectionByteCapacity = sectionBytes;
    request.maxChunkCount = 256;
    request.maxChunksPerGeneration = 4096;
    request.maxChunkBatchesPerGeneration = maxBatches;
    request.replyTimeoutMs = 20'000;
    return request;
}

} // namespace

TEST_CASE("STEP-004 tessellation profile is versioned and deterministically bounded",
          "[step-004][profile]")
{
    using namespace step_host;
    CHECK(kStepTessellationProfileVersion >= 1);
    CHECK(kStepImporterVersion == 1 + kStepTessellationProfileVersion);

    const StepTessellationProfile profile;
    CHECK(profile.displayRelativeDeflection < profile.coarseRelativeDeflection);
    CHECK(profile.displayAngularDeflection < profile.coarseAngularDeflection);

    // Relative deflection scales with the diagonal, then clamps into the
    // absolute window; a non-finite/zero diagonal falls back to the floor.
    CHECK(StepDeriveLinearDeflection(1000.0, 0.003, 0.001, 5.0) == 3.0);
    CHECK(StepDeriveLinearDeflection(1.0, 0.003, 0.001, 5.0) == 0.003);
    CHECK(StepDeriveLinearDeflection(1'000'000.0, 0.003, 0.001, 5.0) == 5.0);
    CHECK(StepDeriveLinearDeflection(0.0, 0.003, 0.001, 5.0) == 0.001);
    CHECK(StepDeriveLinearDeflection(std::nan(""), 0.003, 0.001, 5.0) == 0.001);

    CHECK(StepDeriveMinEdge(1000.0, 0.001, 0.001) == 1.0);
    CHECK(StepDeriveMinEdge(0.0, 0.001, 0.001) == 0.001);

    CHECK(StepWithinDefinitionTime(1.0, 2.0));
    CHECK_FALSE(StepWithinDefinitionTime(3.0, 2.0));
}

TEST_CASE("STEP-004 delivers a bounded scene progressively across output windows",
          "[step-004][progressive]")
{
    const auto result = import_broker::RunImportSession(
        Request(L"assembly_nested_ap214.stp", 0x6400, 24576, 64));
    INFO("code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage)
                 << " batches " << result.batchCount);
    REQUIRE(result.ok);
    // The whole nested assembly does not fit a 4 KiB window, so the host must
    // have handed at least one non-terminal batch to the broker.
    CHECK(result.batchCount >= 2);
    CHECK_FALSE(result.chunks.empty());

    // Every chunk id stays unique across the whole generation.
    std::unordered_set<uint32_t> ids;
    std::uint32_t geometry = 0, nodes = 0, instances = 0;
    for (const auto& chunk : result.chunks) {
        CHECK(ids.insert(chunk.descriptor.chunkId).second);
        switch (chunk.descriptor.topology) {
        case model_core::ChunkTopology::TriangleList: ++geometry; break;
        case model_core::ChunkTopology::Node: ++nodes; break;
        case model_core::ChunkTopology::MeshInstance: ++instances; break;
        default: break;
        }
    }
    CHECK(geometry == 2);
    CHECK(nodes == 8);
    CHECK(instances == 5);
}

TEST_CASE("STEP-004 geometry uses cluster-local positions with a double origin",
          "[step-004][origin]")
{
    const auto result = import_broker::RunImportSession(
        Request(L"assembly_ap214.stp", 0x6410, 64ull * 1024 * 1024, 1));
    REQUIRE(result.ok);
    std::uint32_t geometry = 0;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::TriangleList
            && chunk.descriptor.topology != model_core::ChunkTopology::PointList)
            continue;
        ++geometry;
        // The origin is the componentwise minimum of the emitted positions, so
        // the local bounds are non-negative and touch zero on every axis.
        for (int axis = 0; axis < 3; ++axis) {
            CHECK(std::isfinite(chunk.descriptor.origin[axis]));
            CHECK(chunk.descriptor.localMin[axis] == 0.0f);
            CHECK(chunk.descriptor.localMax[axis] >= 0.0f);
        }
        // The broker already recomputed and matched the instance world bounds.
        CHECK(chunk.descriptor.boundsState == model_core::BoundsState::Verified);
    }
    CHECK(geometry == 2);
}

TEST_CASE("STEP-004 progressive output is deterministic", "[step-004][determinism]")
{
    const auto first = import_broker::RunImportSession(
        Request(L"assembly_ap214.stp", 0x6420, 24576, 64));
    const auto second = import_broker::RunImportSession(
        Request(L"assembly_ap214.stp", 0x6421, 24576, 64));
    REQUIRE(first.ok);
    REQUIRE(second.ok);
    CHECK(first.batchCount == second.batchCount);
    REQUIRE(first.chunks.size() == second.chunks.size());
    for (std::size_t i = 0; i < first.chunks.size(); ++i) {
        CHECK(first.chunks[i].descriptor.chunkId == second.chunks[i].descriptor.chunkId);
        CHECK(first.chunks[i].descriptor.byteSize == second.chunks[i].descriptor.byteSize);
        CHECK(first.chunks[i].descriptor.chunkChecksum == second.chunks[i].descriptor.chunkChecksum);
        CHECK(std::memcmp(first.chunks[i].descriptor.origin, second.chunks[i].descriptor.origin,
                          sizeof(first.chunks[i].descriptor.origin)) == 0);
        CHECK(std::memcmp(first.chunks[i].descriptor.localMin, second.chunks[i].descriptor.localMin,
                          sizeof(first.chunks[i].descriptor.localMin)) == 0);
    }
}



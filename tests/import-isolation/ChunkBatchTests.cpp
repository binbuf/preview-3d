// Progressive delivery: a generation that fills and hands over the output
// window more than once
// (model_core::ControlOpcode::ChunkBatchReady/ChunkBatchConsumed).
//
// Two layers, deliberately kept in one file because they prove two halves of
// the same rule set:
//
//  - Session level. The batch ordering rules, the batch and chunk caps, and
//    the ack handshake live in import_broker::RunImportSession's own reply
//    loop and are reachable only through it, so these cases drive a real
//    AppContainer-sandboxed Preview3DHostileWorker.exe through that function.
//    They are the progressive-delivery half of Gate 3's "the Gate 2 hostile-
//    worker suite is re-run against each newly wired adapter" criterion --
//    re-running the existing four was never going to cover an attack surface
//    that did not exist when they were written.
//
//  - Validator level. Cross-batch chunkId uniqueness and cross-batch
//    dependency resolution live in ValidateAndCopySection's new
//    KnownChunkCatalog parameter, and are far more precisely provable by
//    handing it hand-built sections directly than by teaching a worker to
//    emit them.

#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "import_broker/SharedSectionValidator.h"
#include "model_core/Checksum.h"
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "SandboxTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <vector>

#ifndef PREVIEW3D_TEST_ASSETS_DIR
#error "PREVIEW3D_TEST_ASSETS_DIR must be defined by Tests.ImportIsolation.vcxproj"
#endif

namespace {

using namespace model_core;

// The hostile worker ignores the source file entirely -- it fabricates
// output, which is the only thing a host ever trusts a worker for -- but
// RunImportSession still opens and canonicalizes a real one, so this has to
// exist on disk.
std::wstring AnyRealSourceFile()
{
    return std::wstring(PREVIEW3D_TEST_ASSETS_DIR) + L"tri_tight.glb";
}

// Drives RunImportSession against Preview3DHostileWorker.exe in `mode`.
// Everything else is the shipping configuration, so a rejection below is the
// product's own rule and not a test-only limit.
import_broker::ImportSessionRequest MakeBatchRequest(const wchar_t* mode, uint32_t maxBatches)
{
    import_broker::ImportSessionRequest request;
    request.workerExePath = sandbox_test_support::HostileWorkerExePath();
    request.sourcePath = AnyRealSourceFile();
    request.format = import_broker::ImportFormat::Gltf;
    request.generationId = 4242;
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = import_broker::kImportMaxChunkCount;
    request.maxSidecarRequestsPerGeneration = 64;
    request.maxSidecarFileBytes = 256ull * 1024 * 1024;
    request.maxChunkBatchesPerGeneration = maxBatches;
    request.workerArgumentsOverride = mode;
    return request;
}

// Reads back the marker this suite's hostile worker writes as vertex 0's x,
// so a test can tell which batch a chunk's bytes actually came from.
float MarkerOf(const import_broker::ValidatedChunk& chunk)
{
    VertexPositionOnlyF32 vertex{};
    std::memcpy(&vertex, chunk.payload.data(), sizeof(vertex));
    return vertex.x;
}

} // namespace

// ---------------------------------------------------------------------------
// Session level, against the real sandboxed hostile worker.
// ---------------------------------------------------------------------------

TEST_CASE("A generation delivered across several batches completes with every chunk accepted",
          "[chunk-batch]")
{
    auto result = import_broker::RunImportSession(MakeBatchRequest(L"--batches-honest", 8));

    REQUIRE(result.ok);
    CHECK(result.stage == import_broker::ImportStage::Completed);
    // Two ChunkBatchReady round trips plus the terminal ChunksReady.
    CHECK(result.batchCount == 3);
    REQUIRE(result.chunks.size() == 3);
    // Accumulated in delivery order, so the markers say the batches arrived
    // in the order the worker sent them and none was dropped or duplicated.
    CHECK(MarkerOf(result.chunks[0]) == 10.0f);
    CHECK(MarkerOf(result.chunks[1]) == 20.0f);
    CHECK(MarkerOf(result.chunks[2]) == 30.0f);
}

TEST_CASE("A batch sink receives each batch as it arrives instead of one accumulated result",
          "[chunk-batch]")
{
    std::vector<float> markersInArrivalOrder;
    auto request = MakeBatchRequest(L"--batches-honest", 8);
    request.onBatch = [&](std::vector<import_broker::ValidatedChunk>&& chunks) {
        for (const auto& chunk : chunks) {
            markersInArrivalOrder.push_back(MarkerOf(chunk));
        }
    };

    auto result = import_broker::RunImportSession(request);

    REQUIRE(result.ok);
    CHECK(result.batchCount == 3);
    REQUIRE(markersInArrivalOrder.size() == 3);
    CHECK(markersInArrivalOrder[0] == 10.0f);
    CHECK(markersInArrivalOrder[1] == 20.0f);
    CHECK(markersInArrivalOrder[2] == 30.0f);
    // The whole point of the sink: the host is not also holding the model.
    CHECK(result.chunks.empty());
}

TEST_CASE("A replayed batch index is rejected", "[chunk-batch]")
{
    auto result = import_broker::RunImportSession(MakeBatchRequest(L"--batches-replay-index", 8));

    REQUIRE_FALSE(result.ok);
    CHECK(result.stage == import_broker::ImportStage::ChunkBatchOutOfOrder);
    CHECK(result.errorCode == ImportErrorCode::ImportProtocolViolation);
    // Batch 0 was legitimately accepted before the replay; the count says the
    // rejection happened at the second batch, not the first.
    CHECK(result.batchCount == 1);
}

TEST_CASE("A skipped batch index is rejected", "[chunk-batch]")
{
    auto result = import_broker::RunImportSession(MakeBatchRequest(L"--batches-skip-index", 8));

    REQUIRE_FALSE(result.ok);
    CHECK(result.stage == import_broker::ImportStage::ChunkBatchOutOfOrder);
    CHECK(result.batchCount == 1);
}

TEST_CASE("A chunkId reused from an earlier batch is rejected", "[chunk-batch]")
{
    auto result = import_broker::RunImportSession(MakeBatchRequest(L"--batches-reuse-chunk-id", 8));

    REQUIRE_FALSE(result.ok);
    // Caught by the validator against the catalog, not by the session's own
    // ordering rules -- the batch index itself was correct here.
    CHECK(result.stage == import_broker::ImportStage::ValidateSection);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);
    CHECK(result.batchCount == 1);
}

TEST_CASE("A worker emitting batches without end is stopped at the per-generation cap",
          "[chunk-batch]")
{
    auto result = import_broker::RunImportSession(MakeBatchRequest(L"--batches-unbounded", 4));

    REQUIRE_FALSE(result.ok);
    CHECK(result.stage == import_broker::ImportStage::ChunkBatchLimit);
    // Exactly the cap was accepted, and the cap is what stopped it -- the
    // worker itself never stops.
    CHECK(result.batchCount == 4);
}

TEST_CASE("A worker rewriting the window before its ack can only race itself, never defeat validation",
          "[chunk-batch]")
{
    auto result = import_broker::RunImportSession(MakeBatchRequest(L"--batches-write-before-ack", 8));

    // The worker announces batch 0 and then, without waiting to be told the
    // host is done, rewrites the window 200 times with a different marker.
    //
    // What is NOT asserted, deliberately: which of those writes the host
    // accepted. The host copies the window when it reaches the batch, not at
    // the instant the notice is sent, so a worker that writes before its ack
    // may well have its later content accepted -- it is racing nothing but
    // itself, and every version it wrote is its own. Binding the two would
    // take a content checksum in the notice; that would stop an honest
    // worker's bug, not a hostile worker, which controls both halves anyway.
    //
    // What IS asserted is the property that actually matters and that a
    // concurrent writer could otherwise break: whatever the host accepted
    // passed the full validator, and a mutation landing mid-copy produces a
    // clean rejection rather than a torn chunk. Every chunk below was
    // bounds-checked, checksum-verified and copied into host-owned memory
    // before this call returned.
    REQUIRE(result.ok);
    REQUIRE(result.chunks.size() == 2);
    for (const auto& chunk : result.chunks) {
        CHECK(chunk.descriptor.topology == ChunkTopology::PointList);
        CHECK(chunk.descriptor.vertexCount == 1);
        CHECK(chunk.payload.size() == sizeof(VertexPositionOnlyF32));
    }
    // The second batch is the terminal one, written after the ack and so not
    // subject to the race at all.
    CHECK(MarkerOf(result.chunks[1]) == 20.0f);
}

TEST_CASE("A batch sent after the terminal reply is never serviced", "[chunk-batch]")
{
    auto result = import_broker::RunImportSession(MakeBatchRequest(L"--batches-after-terminal", 8));

    // The terminal ChunksReady ends the generation. The trailing
    // ChunkBatchReady is not an error the host reports -- it is a message
    // nobody is listening for any more, and the count proves it was never
    // turned into a batch. (The marker is not asserted for the same reason as
    // the case above: the worker rewrote the window after announcing the
    // terminal batch, so it raced its own content.)
    REQUIRE(result.ok);
    CHECK(result.batchCount == 1);
    CHECK(result.chunks.size() == 1);
}

TEST_CASE("A batch cap of one permits no progressive delivery", "[chunk-batch]")
{
    auto request = MakeBatchRequest(L"--batches-honest", 8);
    // The default, and what every caller predating progressive delivery gets
    // without changing a line.
    request.maxChunkBatchesPerGeneration = 1;

    auto result = import_broker::RunImportSession(request);

    REQUIRE_FALSE(result.ok);
    CHECK(result.stage == import_broker::ImportStage::ChunkBatchLimit);
    // The cap counts every batch including the terminal one, so the single
    // permitted slot is spent on the worker's first ChunkBatchReady and the
    // generation can then never complete. A single-window import -- one
    // terminal ChunksReady and nothing else -- is exactly what fits, which is
    // what the rest of this suite's 135 cases already exercise.
    CHECK(result.batchCount == 1);
}

// ---------------------------------------------------------------------------
// The real adapter, through the real sandboxed worker: a model that does not
// fit its window crossing in several batches.
//
// The window is shrunk rather than the model grown. sectionByteCapacity is
// already a per-request field, so a small window and a checked-in fixture
// prove the same code path a 350 MiB model would take through the shipping
// 64 MiB one, without committing a 350 MiB file -- the same differential
// shape ImportSessionTests.cpp already uses for the section-size regression.
// ---------------------------------------------------------------------------

namespace {

// A textured fixture, so the batches carry a mesh, a material and an image
// and the cross-batch dependency path is genuinely exercised: with the
// window this small the material lands in a different batch from the image
// it points at, and the mesh in a different batch again from its material.
constexpr const wchar_t* kMultiChunkAsset = L"basisu_textured_triangle.glb";

import_broker::ImportSessionRequest MakeRealWorkerRequest(uint64_t sectionBytes, uint32_t maxBatches)
{
    import_broker::ImportSessionRequest request;
    request.workerExePath = sandbox_test_support::WorkerExePath();
    request.sourcePath = std::wstring(PREVIEW3D_TEST_ASSETS_DIR) + kMultiChunkAsset;
    request.format = import_broker::ImportFormat::Gltf;
    request.generationId = 909;
    request.sectionByteCapacity = sectionBytes;
    request.maxChunkCount = import_broker::kImportMaxChunkCount;
    request.maxSidecarRequestsPerGeneration = 64;
    request.maxSidecarFileBytes = 256ull * 1024 * 1024;
    request.maxChunkBatchesPerGeneration = maxBatches;
    return request;
}

// chunkId -> payload bytes, so two runs can be compared without depending on
// the order the chunks happened to arrive in (which is exactly what changes
// between a single-window and a batched import).
std::map<uint32_t, std::vector<std::byte>> PayloadsById(
    const std::vector<import_broker::ValidatedChunk>& chunks)
{
    std::map<uint32_t, std::vector<std::byte>> byId;
    for (const auto& chunk : chunks) {
        byId[chunk.descriptor.chunkId] = chunk.payload;
    }
    return byId;
}

} // namespace

TEST_CASE("A model too large for its window crosses in several batches with identical content",
          "[chunk-batch]")
{
    // Baseline: the shipping window, which this fixture fits in one section.
    auto single = import_broker::RunImportSession(
        MakeRealWorkerRequest(import_broker::kImportSectionBytes, 1));
    REQUIRE(single.ok);
    CHECK(single.batchCount == 1);
    REQUIRE(single.chunks.size() >= 3); // at least mesh + material + image

    // Size a window that holds the largest single chunk and little else, so
    // the adapter is forced to cut a batch at nearly every chunk. Derived
    // from the fixture rather than hardcoded, so it cannot silently stop
    // forcing multiple batches if the fixture is ever regenerated.
    uint64_t largestPayload = 0;
    for (const auto& chunk : single.chunks) {
        largestPayload = (std::max)(largestPayload, chunk.descriptor.byteSize);
    }
    const uint64_t tinyWindow = kSectionHeaderSize + kChunkDescriptorSize + largestPayload;

    auto batched = import_broker::RunImportSession(MakeRealWorkerRequest(tinyWindow, 64));

    REQUIRE(batched.ok);
    // The point of the whole chunk: this import could not have happened at
    // all before progressive delivery -- the same request would have failed
    // with ResourceLimit.
    CHECK(batched.batchCount > 1);
    CHECK(batched.chunks.size() == single.chunks.size());

    // Same chunks, same ids, same bytes -- only the number of windows they
    // crossed in differs.
    CHECK(PayloadsById(batched.chunks) == PayloadsById(single.chunks));
}

TEST_CASE("The same model in the same small window fails cleanly when batching is not permitted",
          "[chunk-batch]")
{
    auto single = import_broker::RunImportSession(
        MakeRealWorkerRequest(import_broker::kImportSectionBytes, 1));
    REQUIRE(single.ok);
    uint64_t largestPayload = 0;
    for (const auto& chunk : single.chunks) {
        largestPayload = (std::max)(largestPayload, chunk.descriptor.byteSize);
    }
    const uint64_t tinyWindow = kSectionHeaderSize + kChunkDescriptorSize + largestPayload;

    // The differential half: identical file, identical window, and the only
    // difference is that the host will not accept a second batch. This is
    // what every caller saw before this chunk -- a clean ResourceLimit, never
    // a partial or corrupt model.
    auto refused = import_broker::RunImportSession(MakeRealWorkerRequest(tinyWindow, 1));

    REQUIRE_FALSE(refused.ok);
    CHECK(refused.stage == import_broker::ImportStage::ChunkBatchLimit);
}

// ---------------------------------------------------------------------------
// Validator level: the KnownChunkCatalog itself.
// ---------------------------------------------------------------------------

namespace {

struct ChunkSpec {
    ChunkDescriptor descriptor{};
    std::vector<std::byte> payload;
};

// Same shape as SharedSectionValidatorTextureTests.cpp's builder. Duplicated
// rather than shared: that file's copy is in its own anonymous namespace and
// lifting it into a header would make two test files that currently move
// independently move together, for one function.
std::vector<std::byte> BuildSection(std::vector<ChunkSpec> chunks, uint64_t generationId)
{
    uint64_t offset = kSectionHeaderSize + chunks.size() * kChunkDescriptorSize;
    for (auto& c : chunks) {
        c.descriptor.normalizedRangeOffset = offset;
        c.descriptor.normalizedRangeLength = c.payload.size();
        c.descriptor.byteSize = c.payload.size();
        offset += c.payload.size();
    }
    uint64_t sectionLength = offset;

    std::vector<std::byte> section(sectionLength, std::byte{ 0 });
    for (auto& c : chunks) {
        if (!c.payload.empty()) {
            std::memcpy(section.data() + c.descriptor.normalizedRangeOffset, c.payload.data(),
                        c.payload.size());
        }
        c.descriptor.chunkChecksum = Fnv1a64(std::span<const std::byte>(
            section.data() + c.descriptor.normalizedRangeOffset, c.payload.size()));
    }
    for (size_t i = 0; i < chunks.size(); ++i) {
        std::memcpy(section.data() + kSectionHeaderSize + i * kChunkDescriptorSize,
                    &chunks[i].descriptor, sizeof(ChunkDescriptor));
    }

    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = generationId;
    header.sectionLength = sectionLength;
    header.chunkCount = static_cast<uint32_t>(chunks.size());
    header.reserved = 0;
    header.sectionChecksum = Fnv1a64(std::span<const std::byte>(
        section.data() + kSectionHeaderSize, sectionLength - kSectionHeaderSize));
    std::memcpy(section.data(), &header, sizeof(header));
    return section;
}

// One triangle-less PointList chunk, optionally depending on `dependsOn`.
ChunkSpec PointChunk(uint32_t chunkId, uint32_t dependsOn = 0)
{
    ChunkSpec spec;
    spec.descriptor.topology = ChunkTopology::PointList;
    spec.descriptor.vertexCount = 1;
    spec.descriptor.indexCount = 0;
    spec.descriptor.vertexLayoutId = static_cast<uint32_t>(VertexLayoutId::PositionOnly_F32);
    spec.descriptor.chunkId = chunkId;
    if (dependsOn != 0) {
        spec.descriptor.dependencyIds[0] = dependsOn;
        spec.descriptor.dependencyCount = 1;
    }
    VertexPositionOnlyF32 vertex{ 1.0f, 2.0f, 3.0f };
    spec.payload.resize(sizeof(vertex));
    std::memcpy(spec.payload.data(), &vertex, sizeof(vertex));
    return spec;
}

constexpr uint64_t kGenerationId = 77;

} // namespace

TEST_CASE("A dependency on a chunk from an earlier batch resolves through the catalog",
          "[chunk-batch][shared-section-validator]")
{
    auto section = BuildSection({ PointChunk(2, /*dependsOn=*/1) }, kGenerationId);

    // Without the catalog, chunk 1 is simply not in this section.
    auto withoutCatalog = import_broker::ValidateAndCopySection(section, kGenerationId, 8);
    CHECK_FALSE(withoutCatalog.ok);
    CHECK(withoutCatalog.errorCode == ImportErrorCode::MalformedData);

    // With it, the same bytes are accepted -- this is exactly the case that
    // lets a later batch reference a texture an earlier one already carried,
    // instead of re-sending 16 MiB of it per batch.
    import_broker::KnownChunkCatalog catalog{ { 1, ChunkTopology::PointList } };
    auto withCatalog = import_broker::ValidateAndCopySection(section, kGenerationId, 8, &catalog);
    REQUIRE(withCatalog.ok);
    REQUIRE(withCatalog.chunks.size() == 1);
    CHECK(withCatalog.chunks[0].descriptor.chunkId == 2);
}

TEST_CASE("A dependency resolving in neither the section nor the catalog is still rejected",
          "[chunk-batch][shared-section-validator]")
{
    auto section = BuildSection({ PointChunk(2, /*dependsOn=*/999) }, kGenerationId);

    import_broker::KnownChunkCatalog catalog{ { 1, ChunkTopology::PointList } };
    auto result = import_broker::ValidateAndCopySection(section, kGenerationId, 8, &catalog);

    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);
}

TEST_CASE("A chunkId already in the catalog is rejected even though the section itself is consistent",
          "[chunk-batch][shared-section-validator]")
{
    // Internally valid in every way: the only fault is that id 1 was already
    // accepted in an earlier batch of this same generation.
    auto section = BuildSection({ PointChunk(1) }, kGenerationId);

    import_broker::KnownChunkCatalog catalog{ { 1, ChunkTopology::PointList } };
    auto result = import_broker::ValidateAndCopySection(section, kGenerationId, 8, &catalog);

    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == ImportErrorCode::MalformedData);

    // Same bytes with no prior batch: accepted. Proves the rejection above is
    // the catalog's doing and not something else wrong with the section.
    auto fresh = import_broker::ValidateAndCopySection(section, kGenerationId, 8);
    CHECK(fresh.ok);
}

TEST_CASE("A material may depend on an image chunk carried by an earlier batch",
          "[chunk-batch][shared-section-validator]")
{
    MaterialPayload payload{};
    payload.baseColorFactor[0] = payload.baseColorFactor[1] = payload.baseColorFactor[2]
        = payload.baseColorFactor[3] = 1.0f;
    payload.metallicFactor = 1.0f;
    payload.roughnessFactor = 1.0f;
    payload.uvScale[0] = payload.uvScale[1] = 1.0f;
    payload.alphaMode = static_cast<uint32_t>(AlphaModeId::Opaque);

    ChunkSpec material;
    material.descriptor.topology = ChunkTopology::Material;
    material.descriptor.chunkId = 5;
    material.descriptor.dependencyIds[0] = 4; // an Image from the previous batch
    material.descriptor.dependencyCount = 1;
    material.payload.resize(sizeof(payload));
    std::memcpy(material.payload.data(), &payload, sizeof(payload));

    auto section = BuildSection({ material }, kGenerationId);

    // The slot must still resolve to an Image specifically, catalog or not.
    import_broker::KnownChunkCatalog wrongTopology{ { 4, ChunkTopology::PointList } };
    auto rejected = import_broker::ValidateAndCopySection(section, kGenerationId, 8, &wrongTopology);
    CHECK_FALSE(rejected.ok);
    CHECK(rejected.errorCode == ImportErrorCode::MalformedData);

    import_broker::KnownChunkCatalog catalog{ { 4, ChunkTopology::Image } };
    auto accepted = import_broker::ValidateAndCopySection(section, kGenerationId, 8, &catalog);
    REQUIRE(accepted.ok);
    REQUIRE(accepted.chunks.size() == 1);
    CHECK(accepted.chunks[0].descriptor.chunkId == 5);
}

// Gate 2 workstream A, part 2: the import sandbox's data path -- wire
// format, synthetic in-sandbox generator, broker control protocol, and the
// host's copy-then-validate acceptance path. Proves the HONEST-worker
// pipeline end to end; the synthetic hostile-worker suite (adversarial
// mutation/replay/spoofing) is a deferred follow-up, not this file. See
// .docs/design/03-file-formats-and-ingestion.md ("Wire format") and
// .docs/design/02-system-architecture.md (Import worker section).

#include "GenerationLaunchSupport.h"
#include "SandboxTestSupport.h"
#include "import_broker/SandboxLauncher.h"
#include "import_broker/SharedSection.h"
#include "import_broker/SharedSectionValidator.h"
#include "model_core/Checksum.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "model_core/GeometryBounds.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <limits>

namespace {

// Hand-builds a minimal, otherwise-valid section (one PointList chunk of a
// single vertex) with correct checksums, for tests that then deliberately
// corrupt one field to prove the validator rejects it. No worker process
// involved -- these are direct unit tests of ValidateAndCopySection's own
// check sequence, not a substitute for the deferred adversarial
// hostile-worker suite.
std::vector<std::byte> BuildMinimalValidSection(uint64_t generationId)
{
    using namespace model_core;

    VertexPositionOnlyF32 vertex{ 1.0f, 2.0f, 3.0f };
    std::vector<std::byte> vertexBytes(sizeof(vertex));
    std::memcpy(vertexBytes.data(), &vertex, sizeof(vertex));

    size_t payloadOffset = kSectionHeaderSize + kChunkDescriptorSize;
    size_t sectionLength = payloadOffset + vertexBytes.size();

    std::vector<std::byte> section(sectionLength);

    ChunkDescriptor descriptor{};
    descriptor.normalizedRangeOffset = payloadOffset;
    descriptor.normalizedRangeLength = vertexBytes.size();
    descriptor.topology = ChunkTopology::PointList;
    descriptor.indexCount = 0;
    descriptor.vertexCount = 1;
    descriptor.vertexLayoutId = static_cast<uint32_t>(VertexLayoutId::PositionOnly_F32);
    descriptor.lodLevel = 0;
    descriptor.chunkId = 1;
    descriptor.byteSize = vertexBytes.size();
    descriptor.dependencyCount = 0;
    descriptor.chunkChecksum = Fnv1a64(std::span<const std::byte>(vertexBytes));

    SetLocalBounds(descriptor, vertexBytes);
    std::memcpy(section.data() + kSectionHeaderSize, &descriptor, sizeof(descriptor));
    std::memcpy(section.data() + payloadOffset, vertexBytes.data(), vertexBytes.size());

    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = generationId;
    header.scene.generationId = generationId;
    header.sectionLength = sectionLength;
    header.chunkCount = 1;
    header.reserved = 0;
    header.sectionChecksum = Fnv1a64(std::span<const std::byte>(
        section.data() + kSectionHeaderSize, sectionLength - kSectionHeaderSize));

    std::memcpy(section.data(), &header, sizeof(header));

    return section;
}

} // namespace

TEST_CASE("Private geometry validation rejects bounds lies malformed origins metadata and unknown protocols", "[import-pipeline][bounds][metadata]")
{
    for (unsigned attack=0; attack<11; ++attack) {
        CAPTURE(attack);
        auto section = BuildMinimalValidSection(202);
        model_core::SectionHeader header; std::memcpy(&header,section.data(),sizeof(header));
        model_core::ChunkDescriptor descriptor;
        std::memcpy(&descriptor,section.data()+model_core::kSectionHeaderSize,sizeof(descriptor));
        switch (attack) {
        case 0: descriptor.origin[0] = 1e31; break;
        case 1: descriptor.origin[2] = std::numeric_limits<double>::quiet_NaN(); break;
        case 2: descriptor.localMax[0] += 1; break;
        case 3: descriptor.boundsState = model_core::BoundsState::Provisional; break;
        case 4: header.protocolVersion = 0; break;
        case 5: header.protocolVersion = 0xFFFFFFFF; break;
        case 6: header.scene.generationId = 201; break;
        case 7: header.scene.metersPerUnit = std::numeric_limits<double>::infinity(); break;
        case 8: header.scene.nodeCount = 1'000'001; break;
        case 9: descriptor.geometryFlags = 0xFFFFFFFF; break;
        case 10: header.protocolVersion = 2; break;
        }
        std::memcpy(section.data()+model_core::kSectionHeaderSize,&descriptor,sizeof(descriptor));
        header.sectionChecksum = model_core::Fnv1a64(std::span<const std::byte>(section).subspan(model_core::kSectionHeaderSize));
        std::memcpy(section.data(),&header,sizeof(header));
        auto result = import_broker::ValidateAndCopySection(section,202,8);
        CHECK_FALSE(result.ok); CHECK(result.chunks.empty());
    }
}

TEST_CASE("Protocol v3 copied geometry survives a bounded deterministic mutation corpus", "[fuzz][wire-format][bounds]")
{
    uint32_t seed = 0x202B0A7D;
    unsigned accepted = 0, rejected = 0;
    for (unsigned i=0; i<512; ++i) {
        auto section = BuildMinimalValidSection(202);
        seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5;
        if (i%32) section[seed%section.size()] ^= std::byte(uint8_t((seed>>8)|1));
        model_core::SectionHeader header; std::memcpy(&header,section.data(),sizeof(header));
        model_core::ChunkDescriptor descriptor;
        std::memcpy(&descriptor,section.data()+sizeof(header),sizeof(descriptor));
        // Rechecksum bounded ranges so most mutations exercise structural and
        // geometric checks rather than stopping at checksum rejection.
        if (descriptor.normalizedRangeOffset <= section.size()
            && descriptor.byteSize <= section.size()-descriptor.normalizedRangeOffset) {
            descriptor.chunkChecksum = model_core::Fnv1a64(std::span<const std::byte>(section).subspan(
                size_t(descriptor.normalizedRangeOffset),size_t(descriptor.byteSize)));
            std::memcpy(section.data()+sizeof(header),&descriptor,sizeof(descriptor));
        }
        if (header.sectionLength >= sizeof(header) && header.sectionLength <= section.size()) {
            header.sectionChecksum = model_core::Fnv1a64(std::span<const std::byte>(section).subspan(
                sizeof(header),size_t(header.sectionLength-sizeof(header))));
            std::memcpy(section.data(),&header,sizeof(header));
        }
        auto result = import_broker::ValidateAndCopySection(section,202,8);
        if (!result.ok) { ++rejected; CHECK(result.chunks.empty()); continue; }
        ++accepted;
        CHECK(header.protocolVersion == model_core::kCurrentProtocolVersion);
        REQUIRE(result.chunks.size() <= 8);
        for (const auto& copied : result.chunks) {
            CHECK(copied.scene.generationId == 202);
            CHECK(copied.descriptor.boundsState == model_core::BoundsState::Verified);
            CHECK(copied.payload.size() <= section.size());
        }
    }
    CHECK(accepted > 0); CHECK(rejected > 0);
}

TEST_CASE("Host can create, map, write, and read back a pagefile-backed shared section "
          "without launching a worker",
          "[import-pipeline]")
{
    auto section = import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);
    REQUIRE(section);

    auto writeView
        = platform::MappedView::Map(section.get(), FILE_MAP_WRITE, import_broker::kSyntheticSectionBytes);
    REQUIRE(writeView);
    REQUIRE(writeView.bytes().size() == import_broker::kSyntheticSectionBytes);

    const char message[] = "shared-section-roundtrip";
    std::memcpy(writeView.bytes().data(), message, sizeof(message));

    auto readView
        = platform::MappedView::Map(section.get(), FILE_MAP_READ, import_broker::kSyntheticSectionBytes);
    REQUIRE(readView);
    REQUIRE(readView.bytes().size() == import_broker::kSyntheticSectionBytes);

    CHECK(std::memcmp(readView.bytes().data(), message, sizeof(message)) == 0);
}

TEST_CASE("SharedSectionValidator rejects a section whose declared length exceeds the mapped view",
          "[import-pipeline]")
{
    auto section = BuildMinimalValidSection(42);

    // Pass a view truncated below what the header's own sectionLength
    // claims, without updating that claim -- the validator must catch this
    // before trusting sectionLength for anything else.
    std::span<const std::byte> truncated(section.data(), section.size() - 1);

    auto result = import_broker::ValidateAndCopySection(truncated, 42, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
}

TEST_CASE("SharedSectionValidator rejects a generationId mismatch", "[import-pipeline]")
{
    auto section = BuildMinimalValidSection(42);

    auto result = import_broker::ValidateAndCopySection(section, /*expectedGenerationId=*/999, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::ImportProtocolViolation);
}

TEST_CASE("SharedSectionValidator rejects an unrecognized protocol version", "[import-pipeline]")
{
    auto section = BuildMinimalValidSection(42);

    model_core::SectionHeader header{};
    std::memcpy(&header, section.data(), sizeof(header));
    header.protocolVersion = 0xFFFFFFFFu;
    std::memcpy(section.data(), &header, sizeof(header));

    auto result = import_broker::ValidateAndCopySection(section, 42, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::ImportProtocolViolation);
}

TEST_CASE("SharedSectionValidator rejects a chunk count exceeding the caller's cap",
          "[import-pipeline]")
{
    auto section = BuildMinimalValidSection(42);

    auto result = import_broker::ValidateAndCopySection(section, 42, /*maxChunkCount=*/0);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::ResourceLimit);
}

TEST_CASE("Real worker launched with --generate produces a section whose header the host can "
          "read after ChunksReady",
          "[import-pipeline]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto section = import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);
    REQUIRE(section);

    auto launch = generation_launch_support::LaunchWorkerWithControlChannel(
        sandbox_test_support::WorkerExePath(), L"--generate", fixture.sid, section.get());
    REQUIRE(launch.has_value());
    REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));

    model_core::StartGenerationRequest request{};
    request.generationId = 100;
    request.sceneVariant = model_core::kSceneVariant_CubeAndPointCluster;
    request.sectionHandleValue = reinterpret_cast<uint64_t>(section.get());
    request.sectionByteCapacity = import_broker::kSyntheticSectionBytes;
    request.maxChunkCount = 8;
    REQUIRE(model_core::WriteControlMessage(launch->controlInWrite.get(),
                                             model_core::ControlOpcode::StartGeneration, &request,
                                             sizeof(request)));

    auto received = model_core::ReadControlMessage(launch->controlOutRead.get());
    REQUIRE(received.has_value());
    REQUIRE(received->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::ChunksReady));

    DWORD waitResult = WaitForSingleObject(launch->proc.process.get(), 5000);
    CHECK(waitResult == WAIT_OBJECT_0);
    DWORD exitCode = 0;
    GetExitCodeProcess(launch->proc.process.get(), &exitCode);
    CHECK(exitCode == 0);

    auto view = platform::MappedView::Map(section.get(), FILE_MAP_READ,
                                           import_broker::kSyntheticSectionBytes);
    REQUIRE(view);

    model_core::SectionHeader header{};
    std::memcpy(&header, view.bytes().data(), sizeof(header));
    CHECK(header.magic == model_core::kSectionMagic);
    CHECK(header.protocolVersion == model_core::kCurrentProtocolVersion);
    CHECK(header.generationId == 100);
    CHECK(header.chunkCount == 2);
}

TEST_CASE("Real worker's synthetic cube+point-cluster generation validates and copies exactly "
          "the expected chunk contents",
          "[import-pipeline]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto section = import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);
    REQUIRE(section);

    auto launch = generation_launch_support::LaunchWorkerWithControlChannel(
        sandbox_test_support::WorkerExePath(), L"--generate", fixture.sid, section.get());
    REQUIRE(launch.has_value());
    REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));

    constexpr uint64_t kGenerationId = 200;
    model_core::StartGenerationRequest request{};
    request.generationId = kGenerationId;
    request.sceneVariant = model_core::kSceneVariant_CubeAndPointCluster;
    request.sectionHandleValue = reinterpret_cast<uint64_t>(section.get());
    request.sectionByteCapacity = import_broker::kSyntheticSectionBytes;
    request.maxChunkCount = 8;
    REQUIRE(model_core::WriteControlMessage(launch->controlInWrite.get(),
                                             model_core::ControlOpcode::StartGeneration, &request,
                                             sizeof(request)));

    auto received = model_core::ReadControlMessage(launch->controlOutRead.get());
    REQUIRE(received.has_value());
    REQUIRE(received->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::ChunksReady));

    DWORD waitResult = WaitForSingleObject(launch->proc.process.get(), 5000);
    REQUIRE(waitResult == WAIT_OBJECT_0);
    DWORD exitCode = 0;
    GetExitCodeProcess(launch->proc.process.get(), &exitCode);
    REQUIRE(exitCode == 0);

    auto view = platform::MappedView::Map(section.get(), FILE_MAP_READ,
                                           import_broker::kSyntheticSectionBytes);
    REQUIRE(view);

    auto result = import_broker::ValidateAndCopySection(view.bytes(), kGenerationId, 8);
    REQUIRE(result.ok);
    REQUIRE(result.chunks.size() == 2);

    const auto& chunk0 = result.chunks[0];
    CHECK(chunk0.descriptor.topology == model_core::ChunkTopology::TriangleList);
    CHECK(chunk0.descriptor.vertexCount == 24);
    CHECK(chunk0.descriptor.indexCount == 36);
    CHECK(chunk0.descriptor.vertexLayoutId
          == static_cast<uint32_t>(model_core::VertexLayoutId::PositionNormalUv0_F32));
    CHECK(chunk0.descriptor.lodLevel == 0);
    CHECK(chunk0.descriptor.chunkId == 1);
    CHECK(chunk0.descriptor.dependencyCount == 0);

    model_core::VertexPositionNormalUv0F32 vertex0{};
    std::memcpy(&vertex0, chunk0.payload.data(), sizeof(vertex0));
    CHECK(vertex0.px == -0.5f);
    CHECK(vertex0.py == -0.5f);
    CHECK(vertex0.pz == -0.5f);
    CHECK(vertex0.nx == 0.0f);
    CHECK(vertex0.ny == 0.0f);
    CHECK(vertex0.nz == -1.0f);
    CHECK(vertex0.u == 0.0f);
    CHECK(vertex0.v == 0.0f);

    uint32_t index0 = 0;
    std::memcpy(&index0, chunk0.payload.data() + 24 * sizeof(model_core::VertexPositionNormalUv0F32),
                sizeof(index0));
    CHECK(index0 == 0);

    const auto& chunk1 = result.chunks[1];
    CHECK(chunk1.descriptor.topology == model_core::ChunkTopology::PointList);
    CHECK(chunk1.descriptor.vertexCount == 8);
    CHECK(chunk1.descriptor.indexCount == 0);
    CHECK(chunk1.descriptor.vertexLayoutId
          == static_cast<uint32_t>(model_core::VertexLayoutId::PositionOnly_F32));
    CHECK(chunk1.descriptor.lodLevel == 1);
    CHECK(chunk1.descriptor.chunkId == 2);
    CHECK(chunk1.descriptor.dependencyCount == 1);
    CHECK(chunk1.descriptor.dependencyIds[0] == 1);
}

TEST_CASE("Real worker reports ResourceLimit when the section is too small for the synthetic scene",
          "[import-pipeline]")
{
    sandbox_test_support::SandboxFixture fixture;

    constexpr SIZE_T kTooSmall = 64;
    auto section = import_broker::CreateSharedSection(kTooSmall);
    REQUIRE(section);

    auto launch = generation_launch_support::LaunchWorkerWithControlChannel(
        sandbox_test_support::WorkerExePath(), L"--generate", fixture.sid, section.get());
    REQUIRE(launch.has_value());
    REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));

    model_core::StartGenerationRequest request{};
    request.generationId = 300;
    request.sceneVariant = model_core::kSceneVariant_CubeAndPointCluster;
    request.sectionHandleValue = reinterpret_cast<uint64_t>(section.get());
    request.sectionByteCapacity = kTooSmall;
    request.maxChunkCount = 8;
    REQUIRE(model_core::WriteControlMessage(launch->controlInWrite.get(),
                                             model_core::ControlOpcode::StartGeneration, &request,
                                             sizeof(request)));

    auto received = model_core::ReadControlMessage(launch->controlOutRead.get());
    REQUIRE(received.has_value());
    REQUIRE(received->header.opcode
            == static_cast<uint32_t>(model_core::ControlOpcode::GenerationError));

    model_core::GenerationErrorNotice notice{};
    REQUIRE(received->payload.size() == sizeof(notice));
    std::memcpy(&notice, received->payload.data(), sizeof(notice));
    CHECK(notice.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::ResourceLimit));

    DWORD waitResult = WaitForSingleObject(launch->proc.process.get(), 5000);
    REQUIRE(waitResult == WAIT_OBJECT_0);
    DWORD exitCode = 0;
    GetExitCodeProcess(launch->proc.process.get(), &exitCode);
    CHECK(exitCode != 0);
}

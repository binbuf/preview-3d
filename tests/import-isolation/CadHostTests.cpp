// T08 -- IGES and OCCT BREP through the existing isolated CAD host.
//
// Positive cases prove the normalized scene contract (typed outcome, chunk
// counts, verified bounds, colors where the source has them). Negative cases
// prove the format-specific preflight rejects truncation/over-budget input
// before OCCT, and that a mismatched request cannot borrow another format's
// reader.

#include <catch2/catch_test_macros.hpp>

#include "CadPreflight.h"
#include "FcstdPreflight.h"

#include "import_broker/ImportSession.h"
#include "import_broker/SharedSectionValidator.h"
#include "model_core/Checksum.h"
#include "model_core/Compatibility.h"
#include "model_core/ControlProtocol.h"
#include "model_core/FormatSelection.h"
#include "model_core/ImportError.h"
#include "model_core/WireFormat.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#ifndef PREVIEW3D_STEP_HOST_EXE
#error "PREVIEW3D_STEP_HOST_EXE must be defined by Tests.ImportIsolation.vcxproj"
#endif
#ifndef PREVIEW3D_CAD_FIXTURES_DIR
#error "PREVIEW3D_CAD_FIXTURES_DIR must be defined by Tests.ImportIsolation.vcxproj"
#endif

namespace {

std::wstring CadFixture(const wchar_t* name)
{
    return std::wstring(PREVIEW3D_CAD_FIXTURES_DIR) + name;
}

std::vector<std::byte> ReadFixture(const wchar_t* name)
{
    const std::wstring path = CadFixture(name);
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    REQUIRE(input);
    input.seekg(0, std::ios::end);
    const std::streamsize size = input.tellg();
    REQUIRE(size > 0);
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    REQUIRE(input.good());
    return bytes;
}

import_broker::ImportSessionRequest Request(import_broker::ImportFormat format,
                                            const wchar_t* fixture, uint64_t generation)
{
    import_broker::ImportSessionRequest request;
    request.format = format;
    request.sourcePath = CadFixture(fixture);
    request.stepHostExePath = PREVIEW3D_STEP_HOST_EXE;
    request.generationId = generation;
    request.sectionByteCapacity = 64ull * 1024 * 1024;
    request.maxChunkCount = 1024;
    request.maxChunksPerGeneration = 1024;
    request.maxChunkBatchesPerGeneration = 1;
    request.replyTimeoutMs = 30'000;
    return request;
}

struct SceneCounts {
    std::uint32_t geometry = 0;
    std::uint32_t materials = 0;
    std::uint32_t nodes = 0;
    std::uint32_t instances = 0;
    std::uint64_t triangles = 0;
    double minX = 0.0;
    double maxX = 0.0;
};

bool HasCompatibilityFeature(const import_broker::ImportSessionResult& result,
                             std::uint32_t featureId)
{
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::CompatibilityReport)
            continue;
        if (chunk.payload.size() != sizeof(model_core::CompatibilityReportPayload)) continue;
        model_core::CompatibilityReportPayload payload{};
        std::memcpy(&payload, chunk.payload.data(), sizeof(payload));
        for (std::uint32_t index = 0; index < payload.recordCount
             && index < model_core::kMaxCompatibilityRecords; ++index) {
            if (payload.records[index].featureId == featureId) return true;
        }
    }
    return false;
}

SceneCounts Tally(const import_broker::ImportSessionResult& result)
{
    SceneCounts counts;
    bool first = true;
    for (const auto& chunk : result.chunks) {
        switch (chunk.descriptor.topology) {
        case model_core::ChunkTopology::TriangleList:
        case model_core::ChunkTopology::PointList:
            ++counts.geometry;
            counts.triangles += chunk.descriptor.indexCount / 3;
            CHECK(chunk.descriptor.vertexCount > 0);
            CHECK(chunk.descriptor.boundsState == model_core::BoundsState::Verified);
            break;
        case model_core::ChunkTopology::Material:
            ++counts.materials;
            break;
        case model_core::ChunkTopology::Node:
            ++counts.nodes;
            break;
        case model_core::ChunkTopology::MeshInstance: {
            ++counts.instances;
            model_core::MeshInstancePayload payload{};
            REQUIRE(chunk.payload.size() == sizeof(payload));
            std::memcpy(&payload, chunk.payload.data(), sizeof(payload));
            if (first || payload.worldMin[0] < counts.minX) counts.minX = payload.worldMin[0];
            if (first || payload.worldMax[0] > counts.maxX) counts.maxX = payload.worldMax[0];
            first = false;
            break;
        }
        default:
            break;
        }
    }
    return counts;
}

// Minimal protocol-v10 section with one ImportStatus chunk, used to prove the
// new format IDs' up-axis/unit policy without launching the host.
std::vector<std::byte> MetadataSection(model_core::SourceFormatId format,
                                       model_core::UpAxisId axis, double metersPerUnit)
{
    using namespace model_core;
    std::vector<std::byte> bytes(kSectionHeaderSize + kChunkDescriptorSize + sizeof(ImportStatusPayload));
    const std::size_t payloadOffset = kSectionHeaderSize + kChunkDescriptorSize;
    ImportStatusPayload status{};
    std::memcpy(bytes.data() + payloadOffset, &status, sizeof(status));

    ChunkDescriptor descriptor{};
    descriptor.topology = ChunkTopology::ImportStatus;
    descriptor.chunkId = 1;
    descriptor.normalizedRangeOffset = payloadOffset;
    descriptor.normalizedRangeLength = descriptor.byteSize = sizeof(status);
    descriptor.chunkChecksum = WireChecksum64(
        std::span<const std::byte>(bytes).subspan(payloadOffset));
    std::memcpy(bytes.data() + kSectionHeaderSize, &descriptor, sizeof(descriptor));

    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = header.scene.generationId = 91;
    header.scene.format = format;
    header.scene.upAxis = axis;
    header.scene.metersPerUnit = metersPerUnit;
    header.chunkCount = 1;
    header.sectionLength = bytes.size();
    header.sectionChecksum = WireChecksum64(
        std::span<const std::byte>(bytes).subspan(kSectionHeaderSize));
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}

} // namespace

TEST_CASE("T08 CAD protocol additions are closed and append-only", "[cad-iges][protocol]")
{
    CHECK(model_core::kCurrentProtocolVersion == 13);
    CHECK(uint32_t(model_core::SourceFormatId::Iges) == 14);
    CHECK(uint32_t(model_core::SourceFormatId::Brep) == 15);
    CHECK(uint32_t(model_core::ControlOpcode::StartStepImportFromFile) == 20);
    CHECK(uint32_t(model_core::ControlOpcode::StartCadImportFromFile) == 22);
    CHECK(sizeof(model_core::ParseStepFileRequest) == 48);
    CHECK(sizeof(model_core::ParseCadFileRequest) == 56);
    CHECK(model_core::IsKnownCadFormat(model_core::CadFormat::Step));
    CHECK(model_core::IsKnownCadFormat(model_core::CadFormat::Iges));
    CHECK(model_core::IsKnownCadFormat(model_core::CadFormat::Brep));
    CHECK_FALSE(model_core::IsKnownCadFormat(static_cast<model_core::CadFormat>(5)));
}

TEST_CASE("T08 IGES admission verifies fixed 80-column sections and bounds",
          "[cad-iges][preflight]")
{
    const auto part = ReadFixture(L"part.igs");
    const auto accepted = step_host::IgesPreflightBytes(part);
    INFO("status " << uint32_t(accepted.status));
    CHECK(accepted.ok());
    CHECK(accepted.entities > 0);
    CHECK(accepted.references > 0);
    CHECK(accepted.unitFlag == 2); // generated fixture declares millimetres

    const auto truncated = ReadFixture(L"truncated.igs");
    CHECK_FALSE(step_host::IgesPreflightBytes(truncated).ok());

    step_host::CadPreflightLimits limits;
    limits.maxRecords = 3;
    const auto overRecords = step_host::IgesPreflightBytes(part, limits);
    CHECK(overRecords.status == step_host::CadPreflightStatus::RecordLimit);

    // A record with a control byte is an encoding failure, not geometry.
    auto corrupted = part;
    if (corrupted.size() > 20) corrupted[10] = std::byte{0x01};
    CHECK(step_host::IgesPreflightBytes(corrupted).status
          == step_host::CadPreflightStatus::UnsupportedEncoding);
}

TEST_CASE("T08 BREP admission verifies the CASCADE shape-set header and table",
          "[cad-brep][preflight]")
{
    const auto part = ReadFixture(L"part.brep");
    const auto accepted = step_host::BrepPreflightBytes(part);
    INFO("status " << uint32_t(accepted.status));
    CHECK(accepted.ok());
    CHECK(accepted.entities > 0);

    const auto malformed = ReadFixture(L"malformed.brep");
    CHECK(step_host::BrepPreflightBytes(malformed).status
          == step_host::CadPreflightStatus::MalformedSyntax);

    const std::string notBrep = "hello, not a shape set\n";
    CHECK(step_host::BrepPreflightBytes(std::span<const std::byte>(
              reinterpret_cast<const std::byte*>(notBrep.data()), notBrep.size())).status
          == step_host::CadPreflightStatus::NotBrep);

    step_host::CadPreflightLimits limits;
    limits.maxBrepShapes = 1;
    CHECK(step_host::BrepPreflightBytes(part, limits).status
          == step_host::CadPreflightStatus::EntityLimit);
}

TEST_CASE("T08 IGES opens through the isolated CAD host with normalized bounds",
          "[cad-iges]")
{
    const auto result = import_broker::RunImportSession(
        Request(import_broker::ImportFormat::Iges, L"part.igs", 0x8001));
    INFO("code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage));
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.chunks.empty());
    CHECK(result.producer == import_broker::ImportProducer::StepHost);
    CHECK(result.chunks.front().scene.format == model_core::SourceFormatId::Iges);
    CHECK(result.chunks.front().scene.upAxis == model_core::UpAxisId::Unknown);
    // The IGES actor normalizes transferred geometry to OCCT's system
    // millimetre; the factor is verified and positive, never guessed from the
    // extension.
    CHECK(result.chunks.front().scene.metersPerUnit == 0.001);

    const SceneCounts counts = Tally(result);
    CHECK(counts.geometry >= 1);
    CHECK(counts.triangles > 0);
    CHECK(counts.instances >= 1);
}

TEST_CASE("T08 IGES preserves assembly transforms and colors",
          "[cad-iges][color][assembly]")
{
    const auto result = import_broker::RunImportSession(
        Request(import_broker::ImportFormat::Iges, L"assembly.igs", 0x8002));
    INFO("code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage));
    REQUIRE(result.ok);
    const SceneCounts counts = Tally(result);
    INFO("geometry " << counts.geometry << " materials " << counts.materials
                     << " instances " << counts.instances);
    CHECK(counts.geometry >= 1);
    CHECK(counts.materials >= 2);
    CHECK(counts.instances >= 2);
    // The second box is translated 40 mm along X; the scene bounds must span
    // both occurrences rather than collapsing to one local definition.
    CHECK(counts.maxX - counts.minX > 30.0);
}

TEST_CASE("T08 BREP opens through the isolated CAD host with explicit unknown units",
          "[cad-brep]")
{
    const auto result = import_broker::RunImportSession(
        Request(import_broker::ImportFormat::Brep, L"part.brep", 0x8011));
    INFO("code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage));
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.chunks.empty());
    CHECK(result.producer == import_broker::ImportProducer::StepHost);
    CHECK(result.chunks.front().scene.format == model_core::SourceFormatId::Brep);
    CHECK(result.chunks.front().scene.upAxis == model_core::UpAxisId::Unknown);
    CHECK(result.chunks.front().scene.metersPerUnit == 0.0);
    const SceneCounts counts = Tally(result);
    CHECK(counts.geometry >= 1);
    CHECK(counts.triangles > 0);
}

TEST_CASE("T08 BREP compound keeps every solid and its world extent", "[cad-brep]")
{
    const auto result = import_broker::RunImportSession(
        Request(import_broker::ImportFormat::Brep, L"compound.brep", 0x8012));
    INFO("code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage));
    REQUIRE(result.ok);
    const SceneCounts counts = Tally(result);
    CHECK(counts.geometry >= 1);
    CHECK(counts.maxX - counts.minX > 25.0);
}

TEST_CASE("T08 malformed and mismatched CAD input fails typed", "[cad-iges][cad-brep][negative]")
{
    SECTION("truncated IGES") {
        const auto result = import_broker::RunImportSession(
            Request(import_broker::ImportFormat::Iges, L"truncated.igs", 0x8021));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode != model_core::ImportErrorCode::InternalImporterFailure);
        CHECK(result.errorCode != model_core::ImportErrorCode::StepHostFailure);
    }
    SECTION("malformed BREP") {
        const auto result = import_broker::RunImportSession(
            Request(import_broker::ImportFormat::Brep, L"malformed.brep", 0x8022));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
    SECTION("IGES request against a STEP body") {
        const auto result = import_broker::RunImportSession(
            Request(import_broker::ImportFormat::Iges, L"..\\stp-spike\\part_ap214.stp", 0x8023));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
    SECTION("STEP request against an IGES body") {
        const auto result = import_broker::RunImportSession(
            Request(import_broker::ImportFormat::Step, L"part.igs", 0x8024));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
}

TEST_CASE("T08 validator enforces IGES/BREP axis and unit rules", "[cad-validator]")
{
    // IGES: Unknown axis plus a positive post-transfer metre factor.
    CHECK(import_broker::ValidateAndCopySection(
              MetadataSection(model_core::SourceFormatId::Iges,
                              model_core::UpAxisId::Unknown, 0.001), 91, 4).ok);
    for (const auto [axis, units] : {
             std::pair{model_core::UpAxisId::Y, 0.001},
             std::pair{model_core::UpAxisId::Z, 0.001},
             std::pair{model_core::UpAxisId::Unknown, 0.0}}) {
        const auto result = import_broker::ValidateAndCopySection(
            MetadataSection(model_core::SourceFormatId::Iges, axis, units), 91, 4);
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }

    // BREP: Unknown axis and the explicit zero "no authored unit" value.
    CHECK(import_broker::ValidateAndCopySection(
              MetadataSection(model_core::SourceFormatId::Brep,
                              model_core::UpAxisId::Unknown, 0.0), 91, 4).ok);
    for (const auto [axis, units] : {
             std::pair{model_core::UpAxisId::Z, 0.0},
             std::pair{model_core::UpAxisId::X, 0.0},
             std::pair{model_core::UpAxisId::Unknown, 0.001}}) {
        const auto result = import_broker::ValidateAndCopySection(
            MetadataSection(model_core::SourceFormatId::Brep, axis, units), 91, 4);
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
}

TEST_CASE("T09 XBF and FCStd protocol identities are append-only", "[cad-xbf][cad-fcstd][protocol]")
{
    CHECK(uint32_t(model_core::SourceFormatId::Xbf) == 16);
    CHECK(uint32_t(model_core::SourceFormatId::Fcstd) == 17);
    CHECK(uint32_t(model_core::CadFormat::Xbf) == 3);
    CHECK(uint32_t(model_core::CadFormat::Fcstd) == 4);
    CHECK(model_core::IsKnownCadFormat(model_core::CadFormat::Xbf));
    CHECK(model_core::IsKnownCadFormat(model_core::CadFormat::Fcstd));
    CHECK_FALSE(model_core::IsKnownCadFormat(static_cast<model_core::CadFormat>(5)));
    CHECK(model_core::ProgIdForFamily(model_core::FormatId::Xbf) != nullptr);
    CHECK(model_core::ProgIdForFamily(model_core::FormatId::Fcstd) != nullptr);
}

TEST_CASE("T09 XBF BINFILE/version admission and truncation", "[cad-xbf][preflight]")
{
    const auto xbf = ReadFixture(L"assembly.xbf");
    const auto accepted = step_host::XbfPreflightBytes(xbf);
    INFO("status " << uint32_t(accepted.status));
    CHECK(accepted.ok());
    CHECK(accepted.lexedBytes == xbf.size());

    // The header scan is intentionally structural: a file cut after a valid
    // BINFILE header passes admission and fails later in the OCCT reader.
    const auto truncated = ReadFixture(L"truncated.xbf");
    CHECK(step_host::XbfPreflightBytes(truncated).ok());

    const auto malformed = ReadFixture(L"malformed.xbf");
    CHECK(step_host::XbfPreflightBytes(malformed).status
          == step_host::CadPreflightStatus::NotXbf);

    std::vector<std::byte> notXbf = xbf;
    notXbf[0] = std::byte{'X'};
    CHECK(step_host::XbfPreflightBytes(notXbf).status
          == step_host::CadPreflightStatus::NotXbf);

    step_host::CadPreflightLimits limits;
    limits.maxBytes = 64;
    CHECK(step_host::XbfPreflightBytes(xbf, limits).status
          == step_host::CadPreflightStatus::SourceLimit);
}

TEST_CASE("T09 FCStd ZIP preflight limits and required parts", "[cad-fcstd][preflight]")
{
    step_host::FcstdPackage package;
    const auto positive = ReadFixture(L"partdesign.FCStd");
    CHECK(step_host::InspectFcstd(positive, &package) == step_host::FcstdPreflightStatus::Ok);
    CHECK_FALSE(package.documentEntry.empty());
    CHECK(package.hasGuiDocument);
    CHECK(step_host::FindFcstdEntry(package, "PartShape.brp") != nullptr);

    const auto truncated = ReadFixture(L"truncated.FCStd");
    CHECK(step_host::InspectFcstd(truncated) == step_host::FcstdPreflightStatus::NotZip);

    const auto zipbomb = ReadFixture(L"zipbomb.FCStd");
    CHECK(step_host::InspectFcstd(zipbomb) == step_host::FcstdPreflightStatus::ExpansionRatio);

    const auto oversized = ReadFixture(L"oversized.FCStd");
    CHECK(step_host::InspectFcstd(oversized) == step_host::FcstdPreflightStatus::EntryTooLarge);

    // A package without Document.xml is not an FCStd document.
    const auto noDocument = ReadFixture(L"nodocument.FCStd");
    CHECK(step_host::InspectFcstd(noDocument)
          == step_host::FcstdPreflightStatus::MissingDocument);
}

TEST_CASE("T09 XBF opens through the isolated CAD host with colors and assembly", "[cad-xbf]")
{
    const auto result = import_broker::RunImportSession(
        Request(import_broker::ImportFormat::Xbf, L"assembly.xbf", 0x9001));
    INFO("code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage));
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.chunks.empty());
    CHECK(result.producer == import_broker::ImportProducer::StepHost);
    CHECK(result.chunks.front().scene.format == model_core::SourceFormatId::Xbf);
    CHECK(result.chunks.front().scene.upAxis == model_core::UpAxisId::Unknown);
    CHECK(result.chunks.front().scene.metersPerUnit > 0.0);

    const SceneCounts counts = Tally(result);
    CHECK(counts.geometry >= 1);
    CHECK(counts.triangles > 0);
    CHECK(counts.instances >= 2);
    CHECK(counts.materials >= 2);
    CHECK(counts.maxX - counts.minX > 30.0);
}

TEST_CASE("T09 FCStd PartDesign shape, color and nested placement", "[cad-fcstd]")
{
    const auto result = import_broker::RunImportSession(
        Request(import_broker::ImportFormat::Fcstd, L"partdesign.FCStd", 0x9011));
    INFO("code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage));
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.chunks.empty());
    CHECK(result.producer == import_broker::ImportProducer::StepHost);
    CHECK(result.chunks.front().scene.format == model_core::SourceFormatId::Fcstd);
    CHECK(result.chunks.front().scene.upAxis == model_core::UpAxisId::Unknown);
    CHECK(result.chunks.front().scene.metersPerUnit == 0.001);

    const SceneCounts counts = Tally(result);
    CHECK(counts.geometry >= 1);
    CHECK(counts.triangles > 0);
    CHECK(counts.instances == 1); // HiddenBase is not rendered.
    CHECK(counts.materials >= 1); // GuiDocument ShapeColor.
    // Body placement 5 mm inside Assembly placement 10 mm along X.
    CHECK(counts.minX > 14.9);
    CHECK(counts.maxX - counts.minX > 29.0);
    CHECK(HasCompatibilityFeature(result, model_core::kCadFeatureFcstdMillimetresAssumed));
}

TEST_CASE("T09 FCStd external link degrades without opening another path", "[cad-fcstd]")
{
    const auto result = import_broker::RunImportSession(
        Request(import_broker::ImportFormat::Fcstd, L"external_link.FCStd", 0x9012));
    INFO("code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage));
    REQUIRE(result.ok);
    const SceneCounts counts = Tally(result);
    CHECK(counts.geometry >= 1);
    CHECK(HasCompatibilityFeature(result,
                                  model_core::kCadFeatureFcstdExternalLinkIgnored));
}

TEST_CASE("T09 malformed XBF/FCStd fail with typed outcomes", "[cad-xbf][cad-fcstd][negative]")
{
    SECTION("truncated XBF")
    {
        const auto result = import_broker::RunImportSession(
            Request(import_broker::ImportFormat::Xbf, L"truncated.xbf", 0x9021));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
    SECTION("malformed XBF header")
    {
        const auto result = import_broker::RunImportSession(
            Request(import_broker::ImportFormat::Xbf, L"malformed.xbf", 0x9027));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
    SECTION("truncated FCStd")
    {
        const auto result = import_broker::RunImportSession(
            Request(import_broker::ImportFormat::Fcstd, L"truncated.FCStd", 0x9022));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
    SECTION("malformed Document.xml")
    {
        const auto result = import_broker::RunImportSession(
            Request(import_broker::ImportFormat::Fcstd, L"malformed.FCStd", 0x9023));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
    SECTION("zip-bomb FCStd")
    {
        const auto result = import_broker::RunImportSession(
            Request(import_broker::ImportFormat::Fcstd, L"zipbomb.FCStd", 0x9024));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::StepHostLimit);
    }
    SECTION("oversized FCStd")
    {
        const auto result = import_broker::RunImportSession(
            Request(import_broker::ImportFormat::Fcstd, L"oversized.FCStd", 0x9025));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::StepHostLimit);
    }
    SECTION("XBF bytes through the FCStd adapter")
    {
        const auto result = import_broker::RunImportSession(
            Request(import_broker::ImportFormat::Fcstd, L"assembly.xbf", 0x9026));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
}

TEST_CASE("T09 XBF/FCStd section metadata rules match T08", "[cad-validator]")
{
    CHECK(import_broker::ValidateAndCopySection(
              MetadataSection(model_core::SourceFormatId::Xbf,
                              model_core::UpAxisId::Unknown, 0.001), 91, 4).ok);
    for (const auto [axis, units] : {
             std::pair{model_core::UpAxisId::Y, 0.001},
             std::pair{model_core::UpAxisId::Unknown, 0.0}}) {
        const auto result = import_broker::ValidateAndCopySection(
            MetadataSection(model_core::SourceFormatId::Xbf, axis, units), 91, 4);
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }

    CHECK(import_broker::ValidateAndCopySection(
              MetadataSection(model_core::SourceFormatId::Fcstd,
                              model_core::UpAxisId::Unknown, 0.001), 91, 4).ok);
    for (const auto [axis, units] : {
             std::pair{model_core::UpAxisId::Z, 0.001},
             std::pair{model_core::UpAxisId::Unknown, 0.0}}) {
        const auto result = import_broker::ValidateAndCopySection(
            MetadataSection(model_core::SourceFormatId::Fcstd, axis, units), 91, 4);
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
}

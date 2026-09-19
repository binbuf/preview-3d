#include <catch2/catch_test_macros.hpp>

#include "StepPart21Preflight.h"
#include "StepTessellationProfile.h"

#include "import_broker/ImportSession.h"
#include "model_core/ImportError.h"
#include "model_core/MaterialPayload.h"
#include "model_core/WireFormat.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
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

std::string Narrow(const wchar_t* text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length > 0 ? length - 1 : 0), '\0');
    if (length > 1)
        WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    return result;
}

import_broker::ImportSessionRequest Request(const wchar_t* fixture, uint64_t generation)
{
    import_broker::ImportSessionRequest request;
    request.format = import_broker::ImportFormat::Step;
    request.sourcePath = StpFixture(fixture);
    request.stepHostExePath = PREVIEW3D_STEP_HOST_EXE;
    request.generationId = generation;
    request.sectionByteCapacity = 64ull * 1024 * 1024;
    request.maxChunkCount = 1024;
    request.maxChunksPerGeneration = 1024;
    request.maxChunkBatchesPerGeneration = 1;
    request.replyTimeoutMs = 20'000;
    return request;
}

struct SceneCounts {
    std::uint32_t geometry = 0;
    std::uint32_t materials = 0;
    std::uint32_t nodes = 0;
    std::uint32_t instances = 0;
    std::uint32_t triangles = 0;
};

SceneCounts Tally(const import_broker::ImportSessionResult& result)
{
    SceneCounts counts;
    for (const auto& chunk : result.chunks) {
        switch (chunk.descriptor.topology) {
        case model_core::ChunkTopology::TriangleList:
        case model_core::ChunkTopology::PointList:
            ++counts.geometry;
            counts.triangles += chunk.descriptor.indexCount / 3;
            CHECK(chunk.descriptor.boundsState == model_core::BoundsState::Verified);
            CHECK(chunk.descriptor.vertexCount > 0);
            break;
        case model_core::ChunkTopology::Material:
            ++counts.materials;
            break;
        case model_core::ChunkTopology::Node:
            ++counts.nodes;
            break;
        case model_core::ChunkTopology::MeshInstance:
            ++counts.instances;
            break;
        default:
            break;
        }
    }
    return counts;
}

std::span<const std::byte> AsBytes(std::string_view text)
{
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

step_host::StepPreflightResult Preflight(std::string_view text)
{
    return step_host::StepPreflightBytes(AsBytes(text));
}

// Every matrix row names its immutable fixture and the exact typed outcome the
// STEP-004/005 pipeline must produce for it. This is the checked-in
// interoperability contract; adding a fixture without a row (or vice versa) is
// a test failure.
struct MatrixRow {
    const wchar_t* fixture;
    const char* ap;
    const char* content;
    bool expectOk;
    model_core::ImportErrorCode expectError;
    std::uint32_t expectGeometry;
    std::uint32_t expectInstances;
};

const MatrixRow kMatrix[] = {
    {L"part_ap203.stp", "AP203", "B-rep analytic solid", true,
     model_core::ImportErrorCode::None, 1, 1},
    {L"part_ap214.stp", "AP214", "B-rep analytic solid", true,
     model_core::ImportErrorCode::None, 1, 1},
    {L"part_ap242.stp", "AP242", "B-rep analytic solid", true,
     model_core::ImportErrorCode::None, 1, 1},
    {L"tessellated_ap242.stp", "AP242", "B-rep plus authored tessellated representation", true,
     model_core::ImportErrorCode::None, 1, 1},
    {L"tessellated_only_ap242.stp", "AP242", "tessellated-only representation (no B-rep)", true,
     model_core::ImportErrorCode::None, 1, 1},
    {L"assembly_ap214.stp", "AP214", "assembly with reusable definitions and colors", true,
     model_core::ImportErrorCode::None, 2, 3},
    {L"assembly_nested_ap214.stp", "AP214", "nested assembly", true,
     model_core::ImportErrorCode::None, 2, 5},
    {L"instance_color_ap214.stp", "AP214", "instance color override", true,
     model_core::ImportErrorCode::None, 1, 3},
    {L"face_color_ap214.stp", "AP214", "face-color seam split", true,
     model_core::ImportErrorCode::None, 2, 2},
    {L"inch_part_ap214.stp", "AP214", "inch-authored length unit", true,
     model_core::ImportErrorCode::None, 1, 1},
    {L"no_geometry_ap242.stp", "AP242", "product metadata with no visual geometry", false,
     model_core::ImportErrorCode::EmptyGeometry, 0, 0},
    {L"unsupported_schema.stp", "unknown", "geometry-free unknown schema (schema is classification only)",
     false, model_core::ImportErrorCode::EmptyGeometry, 0, 0},
    {L"external_document_ap214.stp", "AP214", "required external document declaration", false,
     model_core::ImportErrorCode::UnsupportedRequiredFeature, 0, 0},
    {L"external_document_relative_ap214.stp", "AP214", "relative external document declaration", false,
     model_core::ImportErrorCode::UnsupportedRequiredFeature, 0, 0},
    {L"external_document_absolute_ap214.stp", "AP214",
     "absolute/UNC/URL external document declaration", false,
     model_core::ImportErrorCode::UnsupportedRequiredFeature, 0, 0},
    {L"faceted_invalid_ap242.stp", "AP242", "tessellated representation with an out-of-range triangle index",
     false, model_core::ImportErrorCode::MalformedData, 0, 0},
};

} // namespace

TEST_CASE("STEP-006 interoperability matrix has a named expected result per fixture",
          "[step-006][matrix]")
{
    uint64_t generation = 0x6600;
    for (const MatrixRow& row : kMatrix) {
        const auto result = import_broker::RunImportSession(Request(row.fixture, generation++));
        INFO("fixture " << Narrow(row.fixture) << " ap " << row.ap << " content " << row.content
                        << " ok " << result.ok << " code " << uint32_t(result.errorCode)
                        << " stage " << uint32_t(result.stage));
        REQUIRE(result.producer == import_broker::ImportProducer::StepHost);
        REQUIRE(result.ok == row.expectOk);
        if (row.expectOk) {
            REQUIRE(result.errorCode == model_core::ImportErrorCode::None);
            REQUIRE_FALSE(result.chunks.empty());
            CHECK(result.chunks.front().scene.format == model_core::SourceFormatId::Step);
            CHECK(result.chunks.front().scene.upAxis == model_core::UpAxisId::Unknown);
            CHECK(result.chunks.front().scene.metersPerUnit > 0.0);
            const SceneCounts counts = Tally(result);
            CHECK(counts.geometry == row.expectGeometry);
            CHECK(counts.instances == row.expectInstances);
        } else {
            REQUIRE(result.errorCode == row.expectError);
        }
    }
}

TEST_CASE("STEP-006 accepts an AP242 tessellated representation alongside its B-rep",
          "[step-006][tessellated]")
{
    const auto result = import_broker::RunImportSession(
        Request(L"tessellated_ap242.stp", 0x6620));
    INFO("code " << uint32_t(result.errorCode));
    REQUIRE(result.ok);
    const SceneCounts counts = Tally(result);
    CHECK(counts.geometry == 1);
    CHECK(counts.instances == 1);
    CHECK(counts.triangles > 0);

    // Repeated imports are byte-identical, so the authored triangulation is
    // consumed deterministically rather than resampled.
    const auto second = import_broker::RunImportSession(
        Request(L"tessellated_ap242.stp", 0x6621));
    REQUIRE(second.ok);
    REQUIRE(result.chunks.size() == second.chunks.size());
    for (std::size_t i = 0; i < result.chunks.size(); ++i) {
        CHECK(result.chunks[i].descriptor.chunkId == second.chunks[i].descriptor.chunkId);
        CHECK(result.chunks[i].descriptor.byteSize == second.chunks[i].descriptor.byteSize);
        CHECK(result.chunks[i].descriptor.chunkChecksum == second.chunks[i].descriptor.chunkChecksum);
    }
}

TEST_CASE("STEP-006 healing decision is explicit and output-affecting steps are off",
          "[step-006][healing]")
{
    // The STEP-001 spike used OCCT defaults with no ShapeFix/XSAlgo sequence.
    // STEP-006 compared no healing, a narrowly pinned validation/healing
    // sequence, and broad automatic healing, and adopted no healing: broad
    // repair mutates geometry and makes the visual result
    // producer-dependent, while a pinned sequence had no measured
    // deterministic benefit on the accepted corpus. Invalid geometry must fail
    // typed rather than be silently repaired.
    CHECK(step_host::kStepHealingPolicy == step_host::StepHealingPolicy::None);
}

TEST_CASE("STEP-006 external declaration discovery rejects every path form before OCCT",
          "[step-006][external][no-bypass]")
{
    // A declaration is discovered from the lexical token stream, so the
    // no-go boundary does not depend on the declaration's path shape, quoting,
    // case, or whitespace. None of these ever reaches a filesystem or network
    // call: the preflight is the only component that sees them and it returns
    // a typed result without resolving anything.
    const std::array<std::string_view, 8> declarations{
        "FILE_POPULATION(('AP214'),('x'),($),$);",
        "file_population(('AP214'),('x'),($),$);",
        "  FILE_POPULATION (('AP214'),('x'),($),$);",
        "DOCUMENT_FILE('sub/other.stp',('AUTOMOTIVE_DESIGN'),'x',#1);",
        "DOCUMENT_FILE('C:\\models\\other.stp',('AUTOMOTIVE_DESIGN'),'x',#1);",
        "DOCUMENT_FILE('\\\\server\\share\\other.stp',('AUTOMOTIVE_DESIGN'),'x',#1);",
        "DOCUMENT_FILE('file:///C:/models/other.stp',('AUTOMOTIVE_DESIGN'),'x',#1);",
        "DOCUMENT_FILE('https://example.invalid/other.stp',('AUTOMOTIVE_DESIGN'),'x',#1);",
    };
    for (const std::string_view declaration : declarations) {
        const std::string file = "ISO-10303-21;\nHEADER;\n" + std::string(declaration)
            + "\nENDSEC;\nDATA;\n#1=APPLICATION_CONTEXT('core data');\nENDSEC;\n"
              "END-ISO-10303-21;\n";
        const auto result = Preflight(file);
        INFO(declaration);
        CHECK(result.status == step_host::StepPreflightStatus::ExternalDocument);
        CHECK(result.externalDocuments >= 1);
    }
}

TEST_CASE("STEP-006 declaration discovery is fuzz-hardened against malformed envelopes",
          "[step-006][external][fuzz]")
{
    // Deterministic adversarial prefixes/suffixes around a declaration must
    // never produce a false accept: the scanner fails closed on an
    // unterminated envelope, an unsupported encoding, or a missing terminator
    // before the declaration policy is even consulted.
    constexpr std::string_view kDeclaration =
        "FILE_POPULATION(('AP214'),('x'),($),$);";
    const std::array<std::string_view, 5> wrappers{
        "ISO-10303-21;\nHEADER;\n",                       // missing ENDSEC/DATA/terminator
        "ISO-10303-21;\nHEADER;\n" + std::string(1, '\0'), // embedded NUL
        "PK\x03\x04",                                     // compressed wrapper
        "<?xml version=\"1.0\"?><x/>",                    // XML wrapper
        "ISO-10303-21;\nHEADER;\n/* unterminated\n",      // unterminated comment
    };
    for (const std::string_view wrapper : wrappers) {
        const std::string file = std::string(wrapper) + std::string(kDeclaration);
        const auto result = Preflight(file);
        CHECK_FALSE(result.ok());
    }
}

#include <catch2/catch_test_macros.hpp>

#include "StepPart21Preflight.h"

#include "import_broker/SharedSectionValidator.h"
#include "model_core/Checksum.h"
#include "model_core/ControlProtocol.h"
#include "model_core/ImportError.h"
#include "model_core/WireFormat.h"

#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::span<const std::byte> AsBytes(std::string_view text)
{
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

step_host::StepPreflightResult Preflight(std::string_view text,
                                         step_host::StepPreflightLimits limits = {})
{
    return step_host::StepPreflightBytes(AsBytes(text), limits);
}

constexpr std::string_view kMinimalPart21 =
    "ISO-10303-21;\n"
    "HEADER;\n"
    "FILE_DESCRIPTION((''),'2;1');\n"
    "FILE_NAME('','',(''),(''),'','','');\n"
    "FILE_SCHEMA(('AUTOMOTIVE_DESIGN'));\n"
    "ENDSEC;\n"
    "DATA;\n"
    "#1=APPLICATION_CONTEXT('core data');\n"
    "#2=CARTESIAN_POINT('',(0.,0.,0.));\n"
    "ENDSEC;\n"
    "END-ISO-10303-21;\n";

// Minimal protocol-v10 section carrying one ImportStatus chunk, used to prove
// the STEP scene-metadata unit/up-axis policy without a host launch.
std::vector<std::byte> StepMetadataSection(model_core::UpAxisId axis, double metersPerUnit)
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
    header.scene.format = SourceFormatId::Step;
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

TEST_CASE("STEP-002 extends protocol-v10 with closed additive identities", "[step-002][protocol]")
{
    CHECK(model_core::kCurrentProtocolVersion == 10);
    CHECK(uint32_t(model_core::SourceFormatId::Step) == 13);
    CHECK(uint32_t(model_core::ControlOpcode::StartStepImportFromFile) == 20);
    CHECK(sizeof(model_core::ParseStepFileRequest) == 48);
    CHECK(uint32_t(model_core::ImportErrorCode::StepHostFailure) == 26);
    CHECK(uint32_t(model_core::ImportErrorCode::StepHostLimit) == 27);
    CHECK(uint32_t(model_core::ImportErrorCode::TessellationFailed) == 28);
    CHECK(model_core::IsKnownImportErrorCode(
        uint32_t(model_core::ImportErrorCode::StepHostFailure)));
    CHECK(model_core::IsKnownImportErrorCode(
        uint32_t(model_core::ImportErrorCode::StepHostLimit)));
    CHECK(model_core::IsKnownImportErrorCode(
        uint32_t(model_core::ImportErrorCode::TessellationFailed)));
    CHECK_FALSE(model_core::IsKnownImportErrorCode(
        uint32_t(model_core::ImportErrorCode::TessellationFailed) + 1));
}

TEST_CASE("STEP-002 Part-21 admission accepts the clear-text envelope", "[step-002][preflight]")
{
    const auto result = Preflight(kMinimalPart21);
    CHECK(result.ok());
    CHECK(result.entityRecords == 2);
    CHECK(result.references == 0);
    CHECK(result.dataSections == 1);
    CHECK(result.maxDepth == 2);
    CHECK(result.externalDocuments == 0);
    CHECK(std::string(result.schema) == "AUTOMOTIVE_DESIGN");
    CHECK(result.lexedBytes == kMinimalPart21.size());
}

TEST_CASE("STEP-002 Part-21 admission honours strings and comments", "[step-002][preflight]")
{
    constexpr std::string_view text =
        "ISO-10303-21;\n"
        "HEADER;\n"
        "FILE_NAME('a;b','c;d',(''),(''),'','','');\n"
        "ENDSEC;\n"
        "DATA;\n"
        "#1=/* a ; comment */CARTESIAN_POINT('semi;colon',(1.,2.,3.));\n"
        "#2=CARTESIAN_POINT('quote''inside',(0.,0.,0.));\n"
        "ENDSEC;\n"
        "END-ISO-10303-21;\n";
    const auto result = Preflight(text);
    CHECK(result.ok());
    CHECK(result.entityRecords == 2);
}

TEST_CASE("STEP-002 Part-21 admission rejects malformed and unsupported encodings",
          "[step-002][preflight][negative]")
{
    SECTION("non Part-21 bytes") {
        CHECK(Preflight("not a step file at all").status
              == step_host::StepPreflightStatus::NotPart21);
    }
    SECTION("empty") {
        CHECK(Preflight("").status == step_host::StepPreflightStatus::NotPart21);
    }
    SECTION("unterminated string") {
        CHECK(Preflight("ISO-10303-21;\nHEADER;\nFILE_NAME('x;").status
              == step_host::StepPreflightStatus::MalformedSyntax);
    }
    SECTION("unterminated comment") {
        CHECK(Preflight("ISO-10303-21;\nHEADER;\n/* never closed").status
              == step_host::StepPreflightStatus::MalformedSyntax);
    }
    SECTION("missing terminator") {
        CHECK(Preflight("ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n#1=A('');\nENDSEC;\n").status
              == step_host::StepPreflightStatus::MalformedSyntax);
    }
    SECTION("duplicate entity identifier") {
        CHECK(Preflight("ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n#1=A('');\n#1=B('');\nENDSEC;\n"
                        "END-ISO-10303-21;\n").status
              == step_host::StepPreflightStatus::DuplicateEntity);
    }
    SECTION("reserved zero entity identifier") {
        CHECK(Preflight("ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n#0=A('');\nENDSEC;\n"
                        "END-ISO-10303-21;\n").status
              == step_host::StepPreflightStatus::DuplicateEntity);
    }
    SECTION("compressed package") {
        std::string bytes("\x50\x4B\x03\x04", 4);
        CHECK(step_host::StepPreflightBytes(AsBytes(bytes)).status
              == step_host::StepPreflightStatus::UnsupportedEncoding);
    }
    SECTION("XML") {
        CHECK(Preflight("<?xml version=\"1.0\"?><root/>").status
              == step_host::StepPreflightStatus::UnsupportedEncoding);
    }
    SECTION("UTF-16 BOM") {
        std::string bytes("\xFF\xFE", 2);
        CHECK(step_host::StepPreflightBytes(AsBytes(bytes)).status
              == step_host::StepPreflightStatus::UnsupportedEncoding);
    }
    SECTION("embedded NUL") {
        std::string bytes = std::string(kMinimalPart21) + std::string("\0", 1);
        CHECK(step_host::StepPreflightBytes(AsBytes(bytes)).status
              == step_host::StepPreflightStatus::UnsupportedEncoding);
    }
}

TEST_CASE("STEP-002 Part-21 admission enforces bounded lexical ceilings",
          "[step-002][preflight][limits]")
{
    SECTION("entity count") {
        step_host::StepPreflightLimits limits;
        limits.maxEntityRecords = 1;
        CHECK(Preflight(kMinimalPart21, limits).status
              == step_host::StepPreflightStatus::EntityLimit);
    }
    SECTION("nesting depth") {
        step_host::StepPreflightLimits limits;
        limits.maxNestingDepth = 4;
        CHECK(Preflight("ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n#1=A((((((1))))));\nENDSEC;\n"
                        "END-ISO-10303-21;\n", limits).status
              == step_host::StepPreflightStatus::DepthLimit);
    }
    SECTION("record length") {
        step_host::StepPreflightLimits limits;
        limits.maxRecordBytes = 64;
        CHECK(Preflight("ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n#1=A('"
                        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx');\n"
                        "ENDSEC;\nEND-ISO-10303-21;\n", limits).status
              == step_host::StepPreflightStatus::RecordLengthLimit);
    }
    SECTION("string length") {
        step_host::StepPreflightLimits limits;
        limits.maxStringBytes = 8;
        CHECK(Preflight("ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n#1=A('longer-than-eight');\n"
                        "ENDSEC;\nEND-ISO-10303-21;\n", limits).status
              == step_host::StepPreflightStatus::StringLengthLimit);
    }
    SECTION("reference count") {
        step_host::StepPreflightLimits limits;
        limits.maxReferenceCount = 1;
        CHECK(Preflight("ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n#1=A(#2,#3);\n#2=B('');\n"
                        "#3=C('');\nENDSEC;\nEND-ISO-10303-21;\n", limits).status
              == step_host::StepPreflightStatus::ReferenceLimit);
    }
    SECTION("source byte cap") {
        step_host::StepPreflightLimits limits;
        limits.maxLexedBytes = 8;
        CHECK(Preflight(kMinimalPart21, limits).status
              == step_host::StepPreflightStatus::SourceLimit);
    }
}

TEST_CASE("STEP-002 Part-21 admission flags external document declarations",
          "[step-002][preflight][external]")
{
    constexpr std::string_view text =
        "ISO-10303-21;\n"
        "HEADER;\n"
        "FILE_POPULATION(('AP214'),('x'),($),$);\n"
        "ENDSEC;\n"
        "DATA;\n"
        "#1=APPLICATION_CONTEXT('core data');\n"
        "ENDSEC;\n"
        "END-ISO-10303-21;\n";
    const auto result = Preflight(text);
    CHECK(result.status == step_host::StepPreflightStatus::ExternalDocument);
    CHECK(result.externalDocuments == 1);
}

TEST_CASE("STEP-002 validator requires unknown up axis and a positive metre factor",
          "[step-002][validator]")
{
    auto accepted = StepMetadataSection(model_core::UpAxisId::Unknown, 1.0);
    CHECK(import_broker::ValidateAndCopySection(accepted, 91, 4).ok);
    auto acceptedMillimetres = StepMetadataSection(model_core::UpAxisId::Unknown, 0.001);
    CHECK(import_broker::ValidateAndCopySection(acceptedMillimetres, 91, 4).ok);

    for (const auto [axis, units] : {
             std::pair{model_core::UpAxisId::Y, 1.0},
             std::pair{model_core::UpAxisId::Z, 1.0},
             std::pair{model_core::UpAxisId::Unknown, 0.0}}) {
        auto bytes = StepMetadataSection(axis, units);
        const auto result = import_broker::ValidateAndCopySection(bytes, 91, 4);
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
}

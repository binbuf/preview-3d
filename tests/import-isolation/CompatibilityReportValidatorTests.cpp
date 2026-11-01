// SharedSectionValidator acceptance rules for ChunkTopology::CompatibilityReport.
// These hand-built sections exercise the validator in isolation -- the same
// copy-then-validate surface a hostile worker would attack.

#include "import_broker/SharedSectionValidator.h"
#include "model_core/Checksum.h"
#include "model_core/Compatibility.h"
#include "model_core/WireFormat.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

namespace {

using namespace model_core;

struct ChunkSpec {
    ChunkDescriptor descriptor{};
    std::vector<std::byte> payload;
};

std::vector<std::byte> BuildSection(std::vector<ChunkSpec> chunks, uint64_t generationId)
{
    uint64_t offset = kSectionHeaderSize + chunks.size() * kChunkDescriptorSize;
    for (auto& chunk : chunks) {
        chunk.descriptor.normalizedRangeOffset = offset;
        chunk.descriptor.normalizedRangeLength = chunk.payload.size();
        chunk.descriptor.byteSize = chunk.payload.size();
        offset += chunk.payload.size();
    }
    const uint64_t sectionLength = offset;

    std::vector<std::byte> section(sectionLength, std::byte{0});
    for (auto& chunk : chunks) {
        if (!chunk.payload.empty())
            std::memcpy(section.data() + chunk.descriptor.normalizedRangeOffset,
                        chunk.payload.data(), chunk.payload.size());
        chunk.descriptor.chunkChecksum = WireChecksum64(std::span<const std::byte>(
            section.data() + chunk.descriptor.normalizedRangeOffset, chunk.payload.size()));
    }
    for (size_t i = 0; i < chunks.size(); ++i)
        std::memcpy(section.data() + kSectionHeaderSize + i * kChunkDescriptorSize,
                    &chunks[i].descriptor, sizeof(ChunkDescriptor));

    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = generationId;
    header.scene.generationId = generationId;
    header.sectionLength = sectionLength;
    header.chunkCount = static_cast<uint32_t>(chunks.size());
    header.sectionChecksum = WireChecksum64(std::span<const std::byte>(
        section.data() + kSectionHeaderSize, sectionLength - kSectionHeaderSize));
    std::memcpy(section.data(), &header, sizeof(header));
    return section;
}

ChunkSpec ReportChunk(const CompatibilityReportPayload& report)
{
    ChunkSpec spec;
    spec.descriptor.topology = ChunkTopology::CompatibilityReport;
    spec.descriptor.chunkId = kCompatibilityReportChunkId;
    spec.payload.resize(sizeof(report));
    std::memcpy(spec.payload.data(), &report, sizeof(report));
    return spec;
}

CompatibilityReportPayload ValidReport()
{
    CompatibilityReportPayload report{};
    report.recordCount = 1;
    report.records[0].featureId = kGltfFeatureClearcoat;
    report.records[0].severity = static_cast<uint8_t>(CompatibilitySeverity::Degraded);
    report.records[0].count = 2;
    return report;
}

import_broker::ValidationResult Validate(const std::vector<std::byte>& section,
                                         uint64_t generationId = 77)
{
    return import_broker::ValidateAndCopySection(section, generationId, 8);
}

} // namespace

TEST_CASE("A well-formed compatibility report is accepted and copied", "[compatibility]")
{
    auto result = Validate(BuildSection({ReportChunk(ValidReport())}, 77));
    REQUIRE(result.ok);
    REQUIRE(result.chunks.size() == 1);
    CHECK(result.chunks[0].descriptor.topology == ChunkTopology::CompatibilityReport);
    CHECK(result.chunks[0].payload.size() == sizeof(CompatibilityReportPayload));
    CompatibilityReportPayload copied{};
    std::memcpy(&copied, result.chunks[0].payload.data(), sizeof(copied));
    CHECK(copied.recordCount == 1);
    CHECK(copied.records[0].featureId == kGltfFeatureClearcoat);
}

TEST_CASE("Compatibility report rejects truncated or oversized payloads", "[compatibility]")
{
    auto valid = ReportChunk(ValidReport());
    auto truncated = valid;
    truncated.payload.resize(sizeof(CompatibilityReportPayload) - 4);
    CHECK_FALSE(Validate(BuildSection({truncated}, 77)).ok);

    auto oversized = valid;
    oversized.payload.resize(sizeof(CompatibilityReportPayload) + 4);
    CHECK_FALSE(Validate(BuildSection({oversized}, 77)).ok);
}

TEST_CASE("Compatibility report rejects an over-cap record count", "[compatibility]")
{
    auto report = ValidReport();
    report.recordCount = kMaxCompatibilityRecords + 1;
    CHECK_FALSE(Validate(BuildSection({ReportChunk(report)}, 77)).ok);
}

TEST_CASE("Compatibility report rejects unknown feature IDs and bad severity",
          "[compatibility]")
{
    auto unknownId = ValidReport();
    unknownId.records[0].featureId = 0;
    CHECK_FALSE(Validate(BuildSection({ReportChunk(unknownId)}, 77)).ok);
    unknownId.records[0].featureId = kFeatureIdContainerMax + 1;
    CHECK_FALSE(Validate(BuildSection({ReportChunk(unknownId)}, 77)).ok);

    auto severity = ValidReport();
    severity.records[0].severity = kCompatibilitySeverityLast + 1;
    CHECK_FALSE(Validate(BuildSection({ReportChunk(severity)}, 77)).ok);
}

TEST_CASE("Compatibility report rejects nonzero reserved bytes and counts out of range",
          "[compatibility]")
{
    auto recordReserved = ValidReport();
    recordReserved.records[0].reserved[0] = 1;
    CHECK_FALSE(Validate(BuildSection({ReportChunk(recordReserved)}, 77)).ok);

    auto payloadReserved = ValidReport();
    payloadReserved.reserved = 1;
    CHECK_FALSE(Validate(BuildSection({ReportChunk(payloadReserved)}, 77)).ok);

    auto zeroCount = ValidReport();
    zeroCount.records[0].count = 0;
    CHECK_FALSE(Validate(BuildSection({ReportChunk(zeroCount)}, 77)).ok);

    auto hugeCount = ValidReport();
    hugeCount.records[0].count = kCompatibilityCountCap + 1;
    CHECK_FALSE(Validate(BuildSection({ReportChunk(hugeCount)}, 77)).ok);
}

TEST_CASE("Compatibility report rejects dependencies and nonzero padding",
          "[compatibility]")
{
    auto dependency = ReportChunk(ValidReport());
    dependency.descriptor.dependencyIds[0] = 1;
    dependency.descriptor.dependencyCount = 1;
    CHECK_FALSE(Validate(BuildSection({dependency}, 77)).ok);

    auto padding = ValidReport();
    padding.records[1].featureId = kGltfFeatureSheen;
    CHECK_FALSE(Validate(BuildSection({ReportChunk(padding)}, 77)).ok);
}

TEST_CASE("A compatibility report may appear at most once per generation",
          "[compatibility]")
{
    auto duplicate = ReportChunk(ValidReport());
    duplicate.descriptor.chunkId = kCompatibilityReportChunkId;
    CHECK_FALSE(Validate(BuildSection({ReportChunk(ValidReport()), duplicate}, 77)).ok);
}

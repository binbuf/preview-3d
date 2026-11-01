#include <catch2/catch_test_macros.hpp>

#include "StepImporterVersion.h"
#include "StepPart21Preflight.h"
#include "StepTessellationProfile.h"

#include "import_broker/ImportSession.h"
#include "model_core/Compatibility.h"
#include "model_core/ControlProtocol.h"
#include "model_core/WireFormat.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
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

std::wstring WriteTempPart21(std::string_view bytes)
{
    wchar_t directory[MAX_PATH]{};
    REQUIRE(GetTempPathW(MAX_PATH, directory) != 0);
    const std::wstring path = std::wstring(directory) + L"preview3d-step-t07-"
        + std::to_wstring(GetCurrentProcessId()) + L"-"
        + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L".stp";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    REQUIRE(output.good());
    return path;
}

struct TempFileGuard {
    std::wstring path;
    ~TempFileGuard() { if (!path.empty()) { std::error_code error; std::filesystem::remove(path, error); } }
};

import_broker::ImportSessionRequest Request(const wchar_t* fixture, uint64_t generation,
                                            model_core::StepImportQuality quality
                                                = model_core::StepImportQuality::Balanced)
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
    request.replyTimeoutMs = 30'000;
    request.stepImportQuality = quality;
    return request;
}

std::span<const std::byte> AsBytes(std::string_view text)
{
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::string Part21(std::string_view body)
{
    return "ISO-10303-21;\nHEADER;\nFILE_SCHEMA(('AUTOMOTIVE_DESIGN'));\nENDSEC;\nDATA;\n"
        + std::string(body) + "ENDSEC;\nEND-ISO-10303-21;\n";
}

std::vector<model_core::CompatibilityRecord> Compatibility(
    const import_broker::ImportSessionResult& result)
{
    std::vector<model_core::CompatibilityRecord> records;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::CompatibilityReport) continue;
        REQUIRE(chunk.payload.size() == sizeof(model_core::CompatibilityReportPayload));
        model_core::CompatibilityReportPayload payload{};
        std::memcpy(&payload, chunk.payload.data(), sizeof(payload));
        REQUIRE(payload.recordCount <= model_core::kMaxCompatibilityRecords);
        for (uint32_t i = 0; i < payload.recordCount; ++i) records.push_back(payload.records[i]);
    }
    return records;
}

bool HasCompatibility(const import_broker::ImportSessionResult& result, uint32_t featureId)
{
    const auto records = Compatibility(result);
    return std::any_of(records.begin(), records.end(),
                       [featureId](const auto& record) { return record.featureId == featureId; });
}

std::uint32_t OptionalWarnings(const import_broker::ImportSessionResult& result)
{
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::ImportStatus) continue;
        if (chunk.payload.size() != sizeof(model_core::ImportStatusPayload)) return 0;
        model_core::ImportStatusPayload payload{};
        std::memcpy(&payload, chunk.payload.data(), sizeof(payload));
        return payload.optionalFeatureWarnings;
    }
    return 0;
}

struct GeometryDigest {
    std::vector<std::uint64_t> checksums;
    std::vector<std::uint32_t> triangleCounts;
};

GeometryDigest Digest(const import_broker::ImportSessionResult& result)
{
    GeometryDigest digest;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::TriangleList
            && chunk.descriptor.topology != model_core::ChunkTopology::PointList)
            continue;
        digest.checksums.push_back(chunk.descriptor.chunkChecksum);
        digest.triangleCounts.push_back(chunk.descriptor.indexCount / 3);
    }
    return digest;
}

} // namespace

TEST_CASE("T07 external declarations are counted only as Part-21 instance names",
          "[step-tolerance][preflight]")
{
    using step_host::StepPreflightStatus;

    // Mentions inside quoted strings and comments are not declarations.
    const std::string quiet = Part21(
        "#1=PRODUCT('FILE_POPULATION','DOCUMENT_FILE','',());\n"
        "/* FILE_POPULATION(('x'),('y'),($),$) */\n");
    const auto quietResult = step_host::StepPreflightBytes(AsBytes(quiet));
    CHECK(quietResult.status == StepPreflightStatus::Ok);
    CHECK(quietResult.externalDocuments == 0);

    // Real declarations are admitted with a count, not rejected.
    const std::string declared = Part21(
        "#1=FILE_POPULATION(('AP214'),('x'),($),$);\n"
        "#2=DOCUMENT_FILE('sub/other.stp',('AUTOMOTIVE_DESIGN'),'x',#1);\n");
    const auto declaredResult = step_host::StepPreflightBytes(AsBytes(declared));
    CHECK(declaredResult.status == StepPreflightStatus::Ok);
    CHECK(declaredResult.externalDocuments == 2);

    // The count itself stays bounded and typed when the cap is exceeded.
    step_host::StepPreflightLimits limits;
    limits.maxExternalDocuments = 1;
    CHECK(step_host::StepPreflightBytes(AsBytes(declared), limits).status
          == StepPreflightStatus::ExternalDocument);
}

TEST_CASE("T07 length-unit policy assumes millimetres only when it must",
          "[step-tolerance][units]")
{
    using namespace step_host;

    const auto absent = ResolveStepLengthUnit(0, 0.001);
    CHECK(absent.assumedMillimetres);
    CHECK_FALSE(absent.contradictory);
    CHECK(absent.metersPerUnit == 0.001);

    const auto unresolvable = ResolveStepLengthUnit(1, 0.0);
    CHECK(unresolvable.assumedMillimetres);
    CHECK(unresolvable.metersPerUnit == 0.001);

    const auto inch = ResolveStepLengthUnit(1, 0.0254);
    CHECK_FALSE(inch.assumedMillimetres);
    CHECK_FALSE(inch.contradictory);
    CHECK(inch.metersPerUnit == 0.0254);

    const auto contradictory = ResolveStepLengthUnit(2, 0.001);
    CHECK(contradictory.contradictory);
    CHECK_FALSE(contradictory.assumedMillimetres);
    CHECK(contradictory.metersPerUnit == 0.001);

    // The existing positive/overflow validation is preserved.
    CHECK(ResolveStepLengthUnit(1, 1e13).overflow);
    CHECK_FALSE(ResolveStepLengthUnit(1, 1e13).assumedMillimetres);
}

TEST_CASE("T07 import quality maps to distinct, versioned deflection profiles",
          "[step-tolerance][quality]")
{
    using namespace step_host;
    const StepTessellationProfile profile;

    CHECK(profile.RelativeDeflection(StepTessellationQuality::Draft)
          == profile.coarseRelativeDeflection);
    CHECK(profile.RelativeDeflection(StepTessellationQuality::Balanced)
          == profile.displayRelativeDeflection);
    CHECK(profile.RelativeDeflection(StepTessellationQuality::Full)
          == profile.fullRelativeDeflection);
    CHECK(profile.AngularDeflection(StepTessellationQuality::Draft) > 0.0);
    CHECK(profile.AngularDeflection(StepTessellationQuality::Full)
          < profile.AngularDeflection(StepTessellationQuality::Balanced));

    // Balanced is exactly the pre-T07 Coarse/Display mapping.
    CHECK(profile.RelativeDeflection(StepTessellationQuality::Coarse)
          == profile.RelativeDeflection(StepTessellationQuality::Draft));
    CHECK(profile.RelativeDeflection(StepTessellationQuality::Display)
          == profile.RelativeDeflection(StepTessellationQuality::Balanced));

    const auto draft = StepImporterCacheIdentity(model_core::StepImportQuality::Draft);
    const auto balanced = StepImporterCacheIdentity(model_core::StepImportQuality::Balanced);
    const auto full = StepImporterCacheIdentity(model_core::StepImportQuality::Full);
    CHECK(draft != balanced);
    CHECK(balanced != full);
    CHECK(draft != full);
    CHECK(kStepTessellationProfileVersion == 6);
    CHECK(kStepImporterVersion == 1 + kStepTessellationProfileVersion);
}

TEST_CASE("T07 request flag quality mapping is closed and defaults to Balanced",
          "[step-tolerance][quality][protocol]")
{
    using namespace model_core;
    StepImportQuality quality = StepImportQuality::Full;
    CHECK(StepImportQualityFromFlags(0, quality));
    CHECK(quality == StepImportQuality::Balanced);
    CHECK(StepImportQualityFromFlags(kImportRequestStepQualityDraft, quality));
    CHECK(quality == StepImportQuality::Draft);
    CHECK(StepImportQualityFromFlags(kImportRequestStepQualityFull, quality));
    CHECK(quality == StepImportQuality::Full);
    CHECK_FALSE(StepImportQualityFromFlags(kImportRequestStepQualityMask, quality));
    CHECK(StepImportQualityFlags(StepImportQuality::Balanced) == 0);
}

TEST_CASE("T07 external-document fixture opens with an unresolved-document warning",
          "[step-tolerance][external]")
{
    const auto result = import_broker::RunImportSession(
        Request(L"external_document_self_contained_ap214.stp", 0x7001));
    INFO("code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage)
                 << " chunks " << result.chunks.size());
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.chunks.empty());
    CHECK(HasCompatibility(result, model_core::kStepFeatureExternalDocumentUnresolved));
    CHECK(OptionalWarnings(result) > 0);
}

TEST_CASE("T07 missing length unit opens with the millimetre assumption and warning",
          "[step-tolerance][units]")
{
    const auto result = import_broker::RunImportSession(
        Request(L"no_unit_ap214.stp", 0x7002));
    INFO("code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage));
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.chunks.empty());
    CHECK(result.chunks.front().scene.metersPerUnit == 0.001);
    CHECK(HasCompatibility(result, model_core::kStepFeatureLengthUnitAssumedMillimetre));
    CHECK(OptionalWarnings(result) > 0);
}

TEST_CASE("T07 contradictory units warn and use the documented first unit factor",
          "[step-tolerance][units]")
{
    const auto result = import_broker::RunImportSession(
        Request(L"contradictory_units_ap214.stp", 0x7003));
    INFO("code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage)
                 << " unit " << (result.chunks.empty() ? 0.0 : result.chunks.front().scene.metersPerUnit));
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.chunks.empty());
    // The source lists millimetres first; the documented dominant choice keeps
    // the pre-T07 millimetre-authored geometry instead of guessing metres.
    CHECK(result.chunks.front().scene.metersPerUnit == 0.001);
    CHECK(HasCompatibility(result, model_core::kStepFeatureLengthUnitContradictory));
    CHECK(OptionalWarnings(result) > 0);
}

TEST_CASE("T07 repair-needed fixture is degraded with a warning or fails typed",
          "[step-tolerance][healing]")
{
    // The fixture is a transferred B-rep whose authored topology needs a small
    // repair. The bounded ShapeFix path either publishes a degraded scene with
    // the repair record or preserves a typed failure; it never silently mutates
    // a successfully tessellated definition.
    const auto result = import_broker::RunImportSession(
        Request(L"repair_needed_ap214.stp", 0x7004));
    INFO("code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage));
    if (result.ok) {
        CHECK(HasCompatibility(result, model_core::kStepFeatureBoundedRepair));
        CHECK(OptionalWarnings(result) > 0);
    } else {
        CHECK(result.errorCode != model_core::ImportErrorCode::InternalImporterFailure);
    }
}

TEST_CASE("T07 unrepairable fixture preserves a typed failure", "[step-tolerance][healing]")
{
    const auto result = import_broker::RunImportSession(
        Request(L"unrepairable_ap214.stp", 0x7005));
    INFO("code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage));
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode != model_core::ImportErrorCode::InternalImporterFailure);
}

TEST_CASE("T07 Draft Balanced Full are deterministic and produce distinct output",
          "[step-tolerance][quality][determinism]")
{
    using model_core::StepImportQuality;
    const auto runs = [](StepImportQuality quality, uint64_t generation) {
        return import_broker::RunImportSession(
            Request(L"part_ap214.stp", generation, quality));
    };

    const auto draft = runs(StepImportQuality::Draft, 0x7010);
    const auto balanced = runs(StepImportQuality::Balanced, 0x7011);
    const auto full = runs(StepImportQuality::Full, 0x7012);
    const auto balancedAgain = runs(StepImportQuality::Balanced, 0x7013);
    const auto draftAgain = runs(StepImportQuality::Draft, 0x7014);
    const auto fullAgain = runs(StepImportQuality::Full, 0x7015);

    REQUIRE(draft.ok);
    REQUIRE(balanced.ok);
    REQUIRE(full.ok);
    REQUIRE(balancedAgain.ok);
    REQUIRE(draftAgain.ok);
    REQUIRE(fullAgain.ok);

    const auto d = Digest(draft);
    const auto b = Digest(balanced);
    const auto f = Digest(full);
    const auto b2 = Digest(balancedAgain);
    const auto d2 = Digest(draftAgain);
    const auto f2 = Digest(fullAgain);

    // Determinism: same quality, same normalized bytes.
    CHECK(d.checksums == d2.checksums);
    CHECK(b.checksums == b2.checksums);
    CHECK(f.checksums == f2.checksums);

    // Distinctness: quality is part of the normalized output identity, so two
    // qualities can never share cached bytes.
    CHECK(d.checksums != b.checksums);
    CHECK(b.checksums != f.checksums);
    CHECK(d.checksums != f.checksums);

    // Finer quality must not emit fewer triangles for this curved part.
    const auto triangles = [](const GeometryDigest& digest) {
        std::uint64_t total = 0;
        for (auto count : digest.triangleCounts) total += count;
        return total;
    };
    CHECK(triangles(f) >= triangles(b));
    CHECK(triangles(b) >= triangles(d));
}

TEST_CASE("T07 malformed and unsafe STEP still fails typed", "[step-tolerance][negative]")
{
    SECTION("nested omitted parameter (SEC-17)") {
        TempFileGuard source{WriteTempPart21(Part21("#1=T('',((,())));\n"))};
        auto request = Request(L"", 0x7020);
        request.sourcePath = source.path;
        const auto result = import_broker::RunImportSession(request);
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
    SECTION("out-of-range authored triangulation") {
        const auto result = import_broker::RunImportSession(
            Request(L"faceted_invalid_ap242.stp", 0x7021));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode != model_core::ImportErrorCode::None);
    }
}

// T06 -- closed per-format import options and the CAD/USD quality surface.
//
// The cases here prove the shared grammar, the packed wire round-trip, the
// broker's pre-launch validation, cache identity, and the real host behaviour
// for read_wire, CAD quality, unit override and USD subdivision.

#include <catch2/catch_test_macros.hpp>

#include "StepImporterVersion.h"

#include "import_broker/ImportSession.h"
#include "model_core/Compatibility.h"
#include "model_core/ControlProtocol.h"
#include "model_core/ImportOptions.h"

#include "SandboxTestSupport.h"
#include "../../interactive-viewer/src/app/ImportOptionsCli.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#ifndef PREVIEW3D_STEP_HOST_EXE
#error "PREVIEW3D_STEP_HOST_EXE must be defined by Tests.ImportIsolation.vcxproj"
#endif
#ifndef PREVIEW3D_IMPORT_HOST_EXE
#error "PREVIEW3D_IMPORT_HOST_EXE must be defined by Tests.ImportIsolation.vcxproj"
#endif
#ifndef PREVIEW3D_STP_FIXTURES_DIR
#error "PREVIEW3D_STP_FIXTURES_DIR must be defined by Tests.ImportIsolation.vcxproj"
#endif
#ifndef PREVIEW3D_CAD_FIXTURES_DIR
#error "PREVIEW3D_CAD_FIXTURES_DIR must be defined by Tests.ImportIsolation.vcxproj"
#endif
#ifndef PREVIEW3D_USD_FIXTURES_DIR
#error "PREVIEW3D_USD_FIXTURES_DIR must be defined by Tests.ImportIsolation.vcxproj"
#endif
#ifndef PREVIEW3D_REPO_ROOT
#error "PREVIEW3D_REPO_ROOT must be defined by Tests.ImportIsolation.vcxproj"
#endif

namespace {

using model_core::ImportOptionError;
using model_core::ImportOptionFormat;
using model_core::ImportOptions;
using model_core::ImportOptionsTable;
using model_core::ImportQuality;
using model_core::ImportWireMode;

std::wstring StpFixture(const wchar_t* name)
{
    return std::wstring(PREVIEW3D_STP_FIXTURES_DIR) + name;
}

std::wstring CadFixture(const wchar_t* name)
{
    return std::wstring(PREVIEW3D_CAD_FIXTURES_DIR) + name;
}

std::wstring UsdFixture(const wchar_t* name)
{
    return std::wstring(PREVIEW3D_USD_FIXTURES_DIR) + name;
}

std::wstring WireFixture(const wchar_t* name)
{
    return std::wstring(PREVIEW3D_REPO_ROOT) + L"tests\\fixtures\\valid-file\\" + name;
}

import_broker::ImportSessionRequest CadRequest(import_broker::ImportFormat format,
                                               std::wstring path, uint64_t generation)
{
    import_broker::ImportSessionRequest request{};
    request.format = format;
    request.sourcePath = std::move(path);
    request.stepHostExePath = PREVIEW3D_STEP_HOST_EXE;
    request.generationId = generation;
    request.sectionByteCapacity = 64ull * 1024 * 1024;
    request.maxChunkCount = 4096;
    request.maxChunksPerGeneration = 4096;
    request.maxChunkBatchesPerGeneration = 4;
    request.replyTimeoutMs = 60'000;
    return request;
}

import_broker::ImportSessionRequest UsdRequest(std::wstring path, uint64_t generation)
{
    import_broker::ImportSessionRequest request{};
    request.format = import_broker::ImportFormat::Usd;
    request.workerExePath = sandbox_test_support::WorkerExePath();
    request.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    request.sourcePath = std::move(path);
    request.generationId = generation;
    request.sectionByteCapacity = 16ull * 1024 * 1024;
    request.maxChunkCount = 1024;
    request.maxChunksPerGeneration = 1024;
    request.maxChunkBatchesPerGeneration = 4;
    request.maxSidecarRequestsPerGeneration = 8;
    request.maxSidecarFileBytes = 64ull * 1024 * 1024;
    request.replyTimeoutMs = 60'000;
    return request;
}

struct Counts {
    uint32_t triangles = 0;
    uint32_t lines = 0;
    uint64_t triangleIndices = 0;
    uint64_t lineIndices = 0;
};

Counts Tally(const import_broker::ImportSessionResult& result)
{
    Counts counts;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList) {
            ++counts.triangles;
            counts.triangleIndices += chunk.descriptor.indexCount;
        } else if (chunk.descriptor.topology == model_core::ChunkTopology::LineList) {
            ++counts.lines;
            counts.lineIndices += chunk.descriptor.indexCount;
        }
    }
    return counts;
}

bool HasCompatibility(const import_broker::ImportSessionResult& result, uint32_t featureId)
{
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::CompatibilityReport)
            continue;
        if (chunk.payload.size() != sizeof(model_core::CompatibilityReportPayload)) continue;
        model_core::CompatibilityReportPayload payload{};
        std::memcpy(&payload, chunk.payload.data(), sizeof(payload));
        for (uint32_t index = 0; index < payload.recordCount
             && index < model_core::kMaxCompatibilityRecords; ++index) {
            if (payload.records[index].featureId == featureId) return true;
        }
    }
    return false;
}

bool Parse(std::string_view text, ImportOptionsTable& table, ImportOptionError& error)
{
    return model_core::ParseImportOptionAssignment(text, table, error);
}

} // namespace

TEST_CASE("T06 option grammar is closed, case-insensitive and range-checked",
          "[import-options][parser]")
{
    ImportOptionsTable table;
    ImportOptionError error = ImportOptionError::None;

    REQUIRE(Parse("step.quality=full", table, error));
    CHECK(table[ImportOptionFormat::Step].quality == ImportQuality::Full);
    REQUIRE(Parse("STEP.Read_Wire=0", table, error));
    CHECK(table[ImportOptionFormat::Step].wireMode == ImportWireMode::Off);
    REQUIRE(Parse("Iges.LINEAR_DEFLECTION=0.25", table, error));
    CHECK(table[ImportOptionFormat::Iges].linearDeflection == 0.25);
    REQUIRE(Parse("BREP.relative_deflection=false", table, error));
    CHECK_FALSE(table[ImportOptionFormat::Brep].relativeDeflection);
    REQUIRE(Parse("FCStd.unit_scale=0.0254", table, error));
    CHECK(table[ImportOptionFormat::Fcstd].unitScaleOverride == 0.0254);
    REQUIRE(Parse("usd.subdivision_level=2", table, error));
    CHECK(table[ImportOptionFormat::Usd].usdSubdivisionLevel == 2);

    CHECK_FALSE(Parse("stp.quality=full", table, error));
    CHECK(error == ImportOptionError::UnknownFormat);
    CHECK_FALSE(Parse("STEP.unknown=1", table, error));
    CHECK(error == ImportOptionError::UnknownName);
    CHECK_FALSE(Parse("USD.read_wire=1", table, error));
    CHECK(error == ImportOptionError::NotApplicable);
    CHECK_FALSE(Parse("USD.quality=full", table, error));
    CHECK(error == ImportOptionError::NotApplicable);
    CHECK_FALSE(Parse("STEP.subdivision_level=1", table, error));
    CHECK(error == ImportOptionError::NotApplicable);
    CHECK_FALSE(Parse("STEP.quality=ultra", table, error));
    CHECK(error == ImportOptionError::InvalidValue);
    CHECK_FALSE(Parse("STEP.linear_deflection=nan", table, error));
    CHECK(error == ImportOptionError::InvalidValue);
    CHECK_FALSE(Parse("STEP.linear_deflection=inf", table, error));
    CHECK(error == ImportOptionError::InvalidValue);
    CHECK_FALSE(Parse("STEP.linear_deflection=0", table, error));
    CHECK(error == ImportOptionError::OutOfRange);
    CHECK_FALSE(Parse("STEP.linear_deflection=0.75", table, error));
    CHECK(error == ImportOptionError::OutOfRange);
    CHECK_FALSE(Parse("STEP.angular_deflection=4", table, error));
    CHECK(error == ImportOptionError::OutOfRange);
    CHECK_FALSE(Parse("STEP.unit_scale=-1", table, error));
    CHECK(error == ImportOptionError::OutOfRange);
    CHECK_FALSE(Parse("USD.subdivision_level=5", table, error));
    CHECK(error == ImportOptionError::OutOfRange);
    CHECK_FALSE(Parse("STEP.quality", table, error));
    CHECK(error == ImportOptionError::InvalidValue);

    // A failed assignment never commits a partial record.
    CHECK(table[ImportOptionFormat::Step].quality == ImportQuality::Full);
    CHECK(table[ImportOptionFormat::Step].wireMode == ImportWireMode::Off);

    // The CLI wrapper accepts the same grammar and rejects non-ASCII input.
    ImportOptionsTable cli;
    REQUIRE(import_options_cli::ParseImportOptionArgument(L"STEP.quality=draft", cli, error));
    CHECK(cli[ImportOptionFormat::Step].quality == ImportQuality::Draft);
    CHECK_FALSE(import_options_cli::ParseImportOptionArgument(L"STEP.quality=\u00e9", cli, error));
    CHECK(error == ImportOptionError::InvalidValue);
}

TEST_CASE("T06 packed option payload round-trips and rejects malformed values",
          "[import-options][protocol]")
{
    ImportOptions options;
    options.quality = ImportQuality::Full;
    options.wireMode = ImportWireMode::On;
    options.linearDeflection = 0.002;
    options.angularDeflection = 0.2;
    options.relativeDeflection = true;
    options.unitScaleOverride = 0.0254;
    const auto payload = model_core::MakeImportOptionsPayload(options);
    CHECK(payload.version == model_core::kImportOptionsPayloadVersion);
    CHECK(sizeof(payload) == 48);

    ImportOptions decoded;
    ImportOptionError error = ImportOptionError::None;
    REQUIRE(model_core::ImportOptionsFromPayload(payload, ImportOptionFormat::Step,
                                                decoded, error));
    CHECK(decoded == options);

    // Legacy zero-filled request: documented defaults, no error.
    model_core::ImportOptionsPayload legacy{};
    REQUIRE(model_core::ImportOptionsFromPayload(legacy, ImportOptionFormat::Step,
                                                 decoded, error));
    CHECK(decoded == ImportOptions{});

    SECTION("unknown version") {
        auto bad = payload;
        bad.version = 99;
        CHECK_FALSE(model_core::ImportOptionsFromPayload(bad, ImportOptionFormat::Step,
                                                         decoded, error));
        CHECK(error == ImportOptionError::InvalidValue);
    }
    SECTION("unknown mask bit") {
        auto bad = payload;
        bad.optionsMask |= 1u << 31;
        CHECK_FALSE(model_core::ImportOptionsFromPayload(bad, ImportOptionFormat::Step,
                                                         decoded, error));
        CHECK(error == ImportOptionError::InvalidValue);
    }
    SECTION("out-of-range enum") {
        auto bad = payload;
        bad.quality = 3;
        CHECK_FALSE(model_core::ImportOptionsFromPayload(bad, ImportOptionFormat::Step,
                                                         decoded, error));
        CHECK(error == ImportOptionError::InvalidValue);
        bad = payload;
        bad.wireMode = 3;
        CHECK_FALSE(model_core::ImportOptionsFromPayload(bad, ImportOptionFormat::Step,
                                                         decoded, error));
        CHECK(error == ImportOptionError::InvalidValue);
    }
    SECTION("non-finite number") {
        auto bad = payload;
        bad.linearDeflection = std::numeric_limits<double>::quiet_NaN();
        CHECK_FALSE(model_core::ImportOptionsFromPayload(bad, ImportOptionFormat::Step,
                                                         decoded, error));
        CHECK(error == ImportOptionError::OutOfRange);
    }
    SECTION("inapplicable field") {
        auto bad = payload;
        bad.optionsMask |= model_core::kImportOptionReadWire;
        bad.wireMode = static_cast<uint32_t>(ImportWireMode::On);
        CHECK_FALSE(model_core::ImportOptionsFromPayload(bad, ImportOptionFormat::Usd,
                                                         decoded, error));
        CHECK(error == ImportOptionError::NotApplicable);
    }
    SECTION("neutral value behind a clear mask bit") {
        auto bad = payload;
        bad.optionsMask &= ~model_core::kImportOptionUnitScale;
        CHECK_FALSE(model_core::ImportOptionsFromPayload(bad, ImportOptionFormat::Step,
                                                         decoded, error));
        CHECK(error == ImportOptionError::InvalidValue);
        bad = payload;
        bad.optionsMask &= ~model_core::kImportOptionQuality;
        CHECK_FALSE(model_core::ImportOptionsFromPayload(bad, ImportOptionFormat::Step,
                                                         decoded, error));
        CHECK(error == ImportOptionError::InvalidValue);
        bad = payload;
        bad.optionsMask &= ~model_core::kImportOptionUsdSubdivision;
        bad.usdSubdivisionLevel = 1;
        CHECK_FALSE(model_core::ImportOptionsFromPayload(bad, ImportOptionFormat::Usd,
                                                         decoded, error));
        CHECK(error == ImportOptionError::InvalidValue);
    }
}

TEST_CASE("T06 cache identity names every output-changing option",
          "[import-options][cache]")
{
    const ImportOptions base;
    const auto baseIdentity = model_core::ImportOptionsCacheIdentity(ImportOptionFormat::Step, base);
    CHECK(baseIdentity == model_core::ImportOptionsCacheIdentity(ImportOptionFormat::Step, base));
    CHECK(baseIdentity != model_core::ImportOptionsCacheIdentity(ImportOptionFormat::Iges, base));

    const auto differs = [&](const ImportOptions& options, ImportOptionFormat format = ImportOptionFormat::Step) {
        return model_core::ImportOptionsCacheIdentity(format, options) != baseIdentity;
    };

    ImportOptions quality;
    quality.quality = ImportQuality::Draft;
    CHECK(differs(quality));
    quality.quality = ImportQuality::Full;
    CHECK(differs(quality));

    ImportOptions wire;
    wire.wireMode = ImportWireMode::On;
    CHECK(differs(wire));
    wire.wireMode = ImportWireMode::Off;
    CHECK(differs(wire));

    ImportOptions linear;
    linear.linearDeflection = 0.001;
    CHECK(differs(linear));
    linear.linearDeflection = 0.002;
    CHECK(differs(linear));

    ImportOptions angular;
    angular.angularDeflection = 0.1;
    CHECK(differs(angular));
    angular.angularDeflection = 0.2;
    CHECK(differs(angular));

    ImportOptions absolute;
    absolute.relativeDeflection = false;
    CHECK(differs(absolute));

    ImportOptions unit;
    unit.unitScaleOverride = 0.001;
    CHECK(differs(unit));
    unit.unitScaleOverride = 0.0254;
    CHECK(differs(unit));

    ImportOptions subdivision;
    subdivision.usdSubdivisionLevel = 1;
    CHECK(differs(subdivision, ImportOptionFormat::Usd));
    subdivision.usdSubdivisionLevel = 2;
    CHECK(differs(subdivision, ImportOptionFormat::Usd));

    // The STEP host folds the same identity into its versioned importer
    // identity, so two option records cannot share a STEP cache entry.
    CHECK(step_host::StepImporterCacheIdentity(ImportOptionFormat::Step, base)
          != step_host::StepImporterCacheIdentity(ImportOptionFormat::Step, quality));
    CHECK(step_host::StepImporterCacheIdentity(ImportOptionFormat::Step, base)
          != step_host::StepImporterCacheIdentity(ImportOptionFormat::Iges, base));
}

TEST_CASE("T06 invalid options fail typed before any parser process runs",
          "[import-options][broker][negative]")
{
    auto request = CadRequest(import_broker::ImportFormat::Step,
                              StpFixture(L"part_ap214.stp"), 0x0600);
    ImportOptions invalid;
    invalid.quality = static_cast<ImportQuality>(9);
    request.importOptions = invalid;
    const auto rejected = import_broker::RunImportSession(request);
    CHECK_FALSE(rejected.ok);
    CHECK(rejected.stage == import_broker::ImportStage::ValidateOptions);
    CHECK(rejected.errorCode == model_core::ImportErrorCode::ImportProtocolViolation);

    // An option record on a format with no product-owned options is rejected,
    // not silently dropped.
    auto gltf = request;
    gltf.format = import_broker::ImportFormat::Gltf;
    gltf.sourcePath = WireFixture(L"wire.step");
    gltf.importOptions = ImportOptions{};
    const auto noFormat = import_broker::RunImportSession(gltf);
    CHECK_FALSE(noFormat.ok);
    CHECK(noFormat.stage == import_broker::ImportStage::ValidateOptions);

    // USD rejects a CAD-only option in the trusted process.
    auto usd = UsdRequest(UsdFixture(L"mesh.usda"), 0x0601);
    ImportOptions usdInvalid;
    usdInvalid.linearDeflection = 0.1;
    usd.importOptions = usdInvalid;
    const auto usdRejected = import_broker::RunImportSession(usd);
    CHECK_FALSE(usdRejected.ok);
    CHECK(usdRejected.stage == import_broker::ImportStage::ValidateOptions);
    CHECK(usdRejected.errorCode == model_core::ImportErrorCode::ImportProtocolViolation);
}

TEST_CASE("T06 read_wire controls CAD fallback and overlay",
          "[import-options][cad][wire]")
{
    SECTION("wireframe: default fallback opens, explicit off is empty") {
        auto fallback = CadRequest(import_broker::ImportFormat::Step,
                                   WireFixture(L"wire.step"), 0x0610);
        const auto fallbackResult = import_broker::RunImportSession(fallback);
        REQUIRE(fallbackResult.ok);
        CHECK(Tally(fallbackResult).lines > 0);
        CHECK(Tally(fallbackResult).triangles == 0);

        auto off = fallback;
        off.generationId = 0x0611;
        off.importOptions = ImportOptions{};
        off.importOptions->wireMode = ImportWireMode::Off;
        const auto offResult = import_broker::RunImportSession(off);
        CHECK_FALSE(offResult.ok);
        CHECK(offResult.errorCode == model_core::ImportErrorCode::EmptyGeometry);

        auto on = fallback;
        on.generationId = 0x0612;
        on.importOptions = ImportOptions{};
        on.importOptions->wireMode = ImportWireMode::On;
        const auto onResult = import_broker::RunImportSession(on);
        REQUIRE(onResult.ok);
        CHECK(Tally(onResult).lines > 0);
    }

    SECTION("solid: fallback has no overlay, explicit on adds edges") {
        auto fallback = CadRequest(import_broker::ImportFormat::Step,
                                   StpFixture(L"part_ap214.stp"), 0x0613);
        const auto fallbackResult = import_broker::RunImportSession(fallback);
        REQUIRE(fallbackResult.ok);
        const Counts fallbackCounts = Tally(fallbackResult);
        CHECK(fallbackCounts.triangles > 0);
        CHECK(fallbackCounts.lines == 0);

        auto on = fallback;
        on.generationId = 0x0614;
        on.importOptions = ImportOptions{};
        on.importOptions->wireMode = ImportWireMode::On;
        const auto onResult = import_broker::RunImportSession(on);
        REQUIRE(onResult.ok);
        const Counts onCounts = Tally(onResult);
        CHECK(onCounts.triangles > 0);
        CHECK(onCounts.lines > 0);

        auto off = fallback;
        off.generationId = 0x0615;
        off.importOptions = ImportOptions{};
        off.importOptions->wireMode = ImportWireMode::Off;
        const auto offResult = import_broker::RunImportSession(off);
        REQUIRE(offResult.ok);
        CHECK(Tally(offResult).triangles > 0);
        CHECK(Tally(offResult).lines == 0);
    }
}

TEST_CASE("T06 Draft and Full quality change CAD output deterministically",
          "[import-options][cad][quality]")
{
    const auto run = [](ImportQuality quality, uint64_t generation) {
        auto request = CadRequest(import_broker::ImportFormat::Step,
                                  StpFixture(L"part_ap214.stp"), generation);
        request.importOptions = ImportOptions{};
        request.importOptions->quality = quality;
        return import_broker::RunImportSession(request);
    };

    const auto draft = run(ImportQuality::Draft, 0x0620);
    const auto balanced = run(ImportQuality::Balanced, 0x0621);
    const auto full = run(ImportQuality::Full, 0x0622);
    const auto draftAgain = run(ImportQuality::Draft, 0x0623);
    const auto fullAgain = run(ImportQuality::Full, 0x0624);
    REQUIRE(draft.ok);
    REQUIRE(balanced.ok);
    REQUIRE(full.ok);
    REQUIRE(draftAgain.ok);
    REQUIRE(fullAgain.ok);

    CHECK(Tally(draft).triangleIndices == Tally(draftAgain).triangleIndices);
    CHECK(Tally(full).triangleIndices == Tally(fullAgain).triangleIndices);
    CHECK(Tally(draft).triangleIndices != Tally(balanced).triangleIndices);
    CHECK(Tally(balanced).triangleIndices != Tally(full).triangleIndices);
    CHECK(Tally(draft).triangleIndices != Tally(full).triangleIndices);

    CHECK(draft.importOptionsCacheIdentity == draftAgain.importOptionsCacheIdentity);
    CHECK(draft.importOptionsCacheIdentity != balanced.importOptionsCacheIdentity);
    CHECK(balanced.importOptionsCacheIdentity != full.importOptionsCacheIdentity);
    CHECK(draft.importOptionsCacheIdentity != full.importOptionsCacheIdentity);
}

TEST_CASE("T06 explicit deflection overrides change CAD output deterministically",
          "[import-options][cad][deflection]")
{
    const auto run = [](const ImportOptions& options, uint64_t generation) {
        auto request = CadRequest(import_broker::ImportFormat::Step,
                                  StpFixture(L"part_ap214.stp"), generation);
        request.importOptions = options;
        return import_broker::RunImportSession(request);
    };

    ImportOptions fine;
    fine.linearDeflection = 0.001;
    fine.angularDeflection = 0.12;
    ImportOptions coarse;
    coarse.linearDeflection = 0.02;
    coarse.angularDeflection = 0.6;

    const auto fineResult = run(fine, 0x0628);
    const auto coarseResult = run(coarse, 0x0629);
    const auto fineAgain = run(fine, 0x062a);
    REQUIRE(fineResult.ok);
    REQUIRE(coarseResult.ok);
    REQUIRE(fineAgain.ok);
    CHECK(Tally(fineResult).triangleIndices == Tally(fineAgain).triangleIndices);
    CHECK(Tally(fineResult).triangleIndices >= Tally(coarseResult).triangleIndices);
    CHECK(Tally(fineResult).triangleIndices != Tally(coarseResult).triangleIndices);

    // The same explicit value in absolute mode also changes the output and is
    // deterministic; this proves relative_deflection is applied, not ignored.
    ImportOptions absoluteFine;
    absoluteFine.relativeDeflection = false;
    absoluteFine.linearDeflection = 0.01;
    ImportOptions absoluteCoarse;
    absoluteCoarse.relativeDeflection = false;
    absoluteCoarse.linearDeflection = 1.0;
    const auto absoluteFineResult = run(absoluteFine, 0x062b);
    const auto absoluteCoarseResult = run(absoluteCoarse, 0x062c);
    REQUIRE(absoluteFineResult.ok);
    REQUIRE(absoluteCoarseResult.ok);
    CHECK(Tally(absoluteFineResult).triangleIndices
          >= Tally(absoluteCoarseResult).triangleIndices);
    CHECK(Tally(absoluteFineResult).triangleIndices
          != Tally(absoluteCoarseResult).triangleIndices);
}

TEST_CASE("T06 unit override changes the reported scale by the expected factor",
          "[import-options][cad][units]")
{
    auto base = CadRequest(import_broker::ImportFormat::Step,
                           StpFixture(L"no_unit_ap214.stp"), 0x0630);
    const auto authored = import_broker::RunImportSession(base);
    REQUIRE(authored.ok);
    REQUIRE_FALSE(authored.chunks.empty());
    CHECK(authored.chunks.front().scene.metersPerUnit == 0.001);
    CHECK(HasCompatibility(authored, model_core::kStepFeatureLengthUnitAssumedMillimetre));

    auto overridden = base;
    overridden.generationId = 0x0631;
    overridden.importOptions = ImportOptions{};
    overridden.importOptions->unitScaleOverride = 0.0254;
    const auto asserted = import_broker::RunImportSession(overridden);
    REQUIRE(asserted.ok);
    REQUIRE_FALSE(asserted.chunks.empty());
    CHECK(asserted.chunks.front().scene.metersPerUnit == 0.0254);
    CHECK(HasCompatibility(asserted, model_core::kCadFeatureUnitOverrideApplied));

    // The normalized geometry is unchanged; only the asserted display scale
    // moves, so the displayed span changes by exactly 25.4x.
    const auto span = [](const import_broker::ImportSessionResult& result) {
        double minimum = 0.0, maximum = 0.0;
        bool first = true;
        for (const auto& chunk : result.chunks) {
            if (chunk.descriptor.topology != model_core::ChunkTopology::MeshInstance) continue;
            model_core::MeshInstancePayload payload{};
            REQUIRE(chunk.payload.size() == sizeof(payload));
            std::memcpy(&payload, chunk.payload.data(), sizeof(payload));
            if (first || payload.worldMin[0] < minimum) minimum = payload.worldMin[0];
            if (first || payload.worldMax[0] > maximum) maximum = payload.worldMax[0];
            first = false;
        }
        REQUIRE_FALSE(first);
        return maximum - minimum;
    };
    const double authoredSpan = span(authored) * authored.chunks.front().scene.metersPerUnit;
    const double assertedSpan = span(asserted) * asserted.chunks.front().scene.metersPerUnit;
    REQUIRE(authoredSpan > 0.0);
    CHECK(std::abs(assertedSpan / authoredSpan - 25.4) < 1e-6);
}

TEST_CASE("T06 USD subdivision is validated and rejected when unsupported",
          "[import-options][usd][subdivision]")
{
    auto base = UsdRequest(UsdFixture(L"mesh.usda"), 0x0640);
    const auto controlCage = import_broker::RunImportSession(base);
    INFO("code " << uint32_t(controlCage.errorCode) << " stage " << uint32_t(controlCage.stage));
    REQUIRE(controlCage.ok);
    CHECK(Tally(controlCage).triangles > 0);

    auto subdivided = base;
    subdivided.generationId = 0x0641;
    subdivided.importOptions = ImportOptions{};
    subdivided.importOptions->usdSubdivisionLevel = 1;
    const auto rejected = import_broker::RunImportSession(subdivided);
    CHECK_FALSE(rejected.ok);
    CHECK(rejected.producer == import_broker::ImportProducer::CompatibilityHost);
    CHECK((rejected.errorCode == model_core::ImportErrorCode::CompatibilityHostFailure
           || rejected.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature));

    auto outOfRange = base;
    outOfRange.generationId = 0x0642;
    outOfRange.importOptions = ImportOptions{};
    outOfRange.importOptions->usdSubdivisionLevel = 99;
    const auto rangeRejected = import_broker::RunImportSession(outOfRange);
    CHECK_FALSE(rangeRejected.ok);
    CHECK(rangeRejected.stage == import_broker::ImportStage::ValidateOptions);
    CHECK(rangeRejected.errorCode == model_core::ImportErrorCode::ImportProtocolViolation);
}

TEST_CASE("T06 session cache identity is present and option-sensitive",
          "[import-options][cache][session]")
{
    auto request = CadRequest(import_broker::ImportFormat::Step,
                              StpFixture(L"part_ap214.stp"), 0x0650);
    request.importOptions = ImportOptions{};
    const auto first = import_broker::RunImportSession(request);
    REQUIRE(first.ok);
    CHECK(first.importOptionsCacheIdentity != 0);

    auto different = request;
    different.generationId = 0x0651;
    different.importOptions->quality = ImportQuality::Draft;
    const auto second = import_broker::RunImportSession(different);
    REQUIRE(second.ok);
    CHECK(second.importOptionsCacheIdentity != 0);
    CHECK(second.importOptionsCacheIdentity != first.importOptionsCacheIdentity);
}

// STEP-008 qualification: frozen corpus typed outcomes and the opt-in
// large-file measurement harness.
//
// The committed corpus manifest (tests/fixtures/step/manifest.json) freezes
// provenance and SHA-256 for the STEP fixtures and their deterministic
// adversarial derivations; tests/fixtures/step/verify.py re-derives and checks
// them. This file is the runtime oracle for the same families: every
// unsupported, over-limit, or malformed admission family has an asserted typed
// outcome, and the opt-in `[.][step-008-measure]` case records the genuine
// 100 MB+ where-time-goes split through the real AppContainer host.

#include <catch2/catch_test_macros.hpp>

#include "StepPart21Preflight.h"

#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "import_broker/SharedSectionValidator.h"
#include "model_core/ControlProtocol.h"
#include "model_core/ImportError.h"
#include "model_core/WireFormat.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

std::span<const std::byte> AsBytes(std::string_view text)
{
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

step_host::StepPreflightResult Preflight(std::string_view text,
                                         step_host::StepPreflightLimits limits = {})
{
    return step_host::StepPreflightBytes(AsBytes(text), limits);
}

// A syntactically valid Part-21 file whose DATA section holds `records`.
std::string Part21(std::string_view records)
{
    std::string text = "ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n";
    text += records;
    text += "\nENDSEC;\nEND-ISO-10303-21;\n";
    return text;
}

std::string Part21WithEntities(std::size_t entities)
{
    std::string records;
    for (std::size_t i = 1; i <= entities; ++i)
        records += "#" + std::to_string(i) + "=CARTESIAN_POINT('',(0.,0.,0.));\n";
    return Part21(records);
}

// `references` `#`-references inside one otherwise valid entity record.
std::string Part21WithReferences(std::size_t references)
{
    std::string body = "#1=PRODUCT('x'";
    for (std::size_t i = 0; i < references; ++i)
        body += ",#" + std::to_string(i + 2);
    body += ");";
    return Part21(body);
}

std::string Part21WithDepth(std::size_t depth)
{
    std::string body = "#1=A";
    body += std::string(depth, '(');
    body += std::string(depth, ')');
    body += ";";
    return Part21(body);
}

std::string Part21WithRecordBytes(std::size_t recordBytes)
{
    // The scanner treats the newline after `DATA;` as part of the next record,
    // so this builds the record with no leading whitespace. The record body
    // excludes the terminating ';': a body of exactly maxRecordBytes is
    // accepted and maxRecordBytes + 1 is rejected.
    const std::string prefix = "#1=";
    return "ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;" + prefix
        + std::string(recordBytes - prefix.size(), 'A') + ";\nENDSEC;\nEND-ISO-10303-21;\n";
}

std::string Part21WithStringBytes(std::size_t stringBytes)
{
    return Part21("#1=PRODUCT('" + std::string(stringBytes, 'A') + "');");
}

struct TypedCase {
    const char* name;
    std::string text;
    step_host::StepPreflightStatus status;
};

} // namespace

TEST_CASE("STEP-008 admission gives every malformed or unsupported family a typed outcome",
          "[step-008][preflight-typed]")
{
    using step_host::StepPreflightStatus;
    const std::vector<TypedCase> cases{
        {"valid", Part21("#1=CARTESIAN_POINT('',(0.,0.,0.));"), StepPreflightStatus::Ok},
        {"empty", "", StepPreflightStatus::NotPart21},
        {"not-part21", "ISO-10303-22;\nHEADER;\nENDSEC;\nDATA;\nENDSEC;\nEND-ISO-10303-22;\n",
         StepPreflightStatus::NotPart21},
        {"utf16-bom", std::string("\xff\xfe", 2) + Part21(""),
         StepPreflightStatus::UnsupportedEncoding},
        {"zip-signature", std::string("PK\x03\x04", 4) + Part21(""),
         StepPreflightStatus::UnsupportedEncoding},
        {"xml-signature", "<?xml version=\"1.0\"?><x/>", StepPreflightStatus::UnsupportedEncoding},
        {"embedded-nul", Part21(std::string("#1=A\x00;", 6)), StepPreflightStatus::UnsupportedEncoding},
        {"truncate-terminator",
         "ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n#1=CARTESIAN_POINT('',(0.,0.,0.));\nENDSEC;\n",
         StepPreflightStatus::MalformedSyntax},
        {"unterminated-string", Part21("#999999=PRODUCT('oops;\n"),
         StepPreflightStatus::MalformedSyntax},
        {"unterminated-comment", Part21("/* oops\n"), StepPreflightStatus::MalformedSyntax},
        {"duplicate-entity", Part21("#1=A;\n#1=B;\n"), StepPreflightStatus::DuplicateEntity},
        {"zero-entity-id", Part21("#0=A;\n"), StepPreflightStatus::DuplicateEntity},
        {"impossible-entity-id", Part21("#99999999999=A;\n"), StepPreflightStatus::DuplicateEntity},
        {"external-file-population",
         Part21("#1=FILE_POPULATION(('AP214'),('x'),($),$);\n"),
         StepPreflightStatus::ExternalDocument},
        {"external-document-relative",
         Part21("#1=DOCUMENT_FILE('sub/other.stp',('AUTOMOTIVE_DESIGN'),'x',#2);\n"),
         StepPreflightStatus::ExternalDocument},
    };
    for (const TypedCase& item : cases) {
        const auto result = Preflight(item.text);
        INFO(item.name << " -> " << uint32_t(result.status));
        CHECK(result.status == item.status);
    }
}

TEST_CASE("STEP-008 admission caps are exact at the boundary and typed just over",
          "[step-008][caps]")
{
    using step_host::StepPreflightStatus;

    // Entity records.
    {
        step_host::StepPreflightLimits limits;
        limits.maxEntityRecords = 256;
        CHECK(Preflight(Part21WithEntities(256), limits).status == StepPreflightStatus::Ok);
        const auto over = Preflight(Part21WithEntities(257), limits);
        CHECK(over.status == StepPreflightStatus::EntityLimit);
        CHECK(over.entityRecords == 257);
    }
    // References.
    {
        step_host::StepPreflightLimits limits;
        limits.maxReferenceCount = 64;
        CHECK(Preflight(Part21WithReferences(64), limits).status == StepPreflightStatus::Ok);
        CHECK(Preflight(Part21WithReferences(65), limits).status
              == StepPreflightStatus::ReferenceLimit);
    }
    // Parenthesis nesting depth.
    {
        step_host::StepPreflightLimits limits;
        limits.maxNestingDepth = 8;
        CHECK(Preflight(Part21WithDepth(8), limits).status == StepPreflightStatus::Ok);
        CHECK(Preflight(Part21WithDepth(9), limits).status == StepPreflightStatus::DepthLimit);
    }
    // One record's byte length.
    {
        step_host::StepPreflightLimits limits;
        limits.maxRecordBytes = 64;
        limits.maxStringBytes = 1 << 20;
        CHECK(Preflight(Part21WithRecordBytes(64), limits).status == StepPreflightStatus::Ok);
        CHECK(Preflight(Part21WithRecordBytes(65), limits).status
              == StepPreflightStatus::RecordLengthLimit);
    }
    // One string literal's byte length.
    {
        step_host::StepPreflightLimits limits;
        limits.maxStringBytes = 64;
        CHECK(Preflight(Part21WithStringBytes(64), limits).status == StepPreflightStatus::Ok);
        CHECK(Preflight(Part21WithStringBytes(65), limits).status
              == StepPreflightStatus::StringLengthLimit);
    }
    // Total lexed source bytes.
    {
        const std::string valid = Part21("#1=CARTESIAN_POINT('',(0.,0.,0.));");
        step_host::StepPreflightLimits exact;
        exact.maxLexedBytes = valid.size();
        CHECK(Preflight(valid, exact).status == StepPreflightStatus::Ok);
        step_host::StepPreflightLimits short_;
        short_.maxLexedBytes = valid.size() - 1;
        CHECK(Preflight(valid, short_).status == StepPreflightStatus::SourceLimit);
    }
    // DATA sections.
    {
        step_host::StepPreflightLimits limits;
        limits.maxDataSections = 1;
        const std::string twoSections =
            "ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n#1=A;\nENDSEC;\n"
            "DATA;\n#2=B;\nENDSEC;\nEND-ISO-10303-21;\n";
        CHECK(Preflight(twoSections, limits).status == StepPreflightStatus::SourceLimit);
    }
    // External-document declarations.
    {
        step_host::StepPreflightLimits limits;
        limits.maxExternalDocuments = 1;
        CHECK(Preflight(Part21("#1=FILE_POPULATION(('AP214'),('x'),($),$);\n"), limits).status
              == StepPreflightStatus::Ok);
        const std::string twoDeclarations = Part21(
            "#1=FILE_POPULATION(('AP214'),('x'),($),$);\n"
            "#2=DOCUMENT_FILE('sub/other.stp',('AUTOMOTIVE_DESIGN'),'x',#3);\n");
        CHECK(Preflight(twoDeclarations, limits).status == StepPreflightStatus::ExternalDocument);
    }
}

// A many-definition scene whose geometry is small must be split into batches
// that respect the broker's per-section chunk cap, not accumulated into one
// oversized window. Before STEP-008 the emitter only flushed on byte capacity,
// so this import was rejected as ResourceLimit instead of being delivered.
TEST_CASE("STEP-008 bounds every progressive batch by maxChunkCount",
          "[step-008][chunk-cap]")
{
    import_broker::ImportSessionRequest request;
    request.format = import_broker::ImportFormat::Step;
    request.sourcePath = std::wstring(PREVIEW3D_STP_FIXTURES_DIR) + L"assembly_nested_ap214.stp";
    request.stepHostExePath = PREVIEW3D_STEP_HOST_EXE;
    request.generationId = 0x6810;
    request.sectionByteCapacity = 64ull * 1024 * 1024;
    request.maxChunkCount = 4;
    request.maxChunkBatchesPerGeneration = 64;
    request.maxChunksPerGeneration = 0;
    request.replyTimeoutMs = 30'000;

    std::uint32_t batches = 0;
    std::uint32_t chunks = 0;
    request.onBatch = [&](std::vector<import_broker::ValidatedChunk>&& batch) {
        ++batches;
        chunks += static_cast<std::uint32_t>(batch.size());
        CHECK(batch.size() <= 4);
    };
    const auto result = import_broker::RunImportSession(request);
    INFO("code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage)
                 << " batches " << batches << " chunks " << chunks);
    REQUIRE(result.ok);
    CHECK(batches > 1);
    CHECK(result.batchCount == batches);
    CHECK(chunks > 4);
}

// Opt-in genuine large-file measurement. It needs the local, non-redistributed
// corpus named in tests/fixtures/step/manifest.json. Run explicitly:
//
//   set PREVIEW3D_MANUAL_STEP_FILE=<abs path to Voron_2.4r2_Assembly.step>
//   x64/Release/Tests.ImportIsolation.exe "[.][step-008-measure]"
//
// It prints the where-time-goes split, observed peak host private commit at
// batch boundaries, and the delivered geometry totals. The ordinary
// [step-008] selector skips it because it is a measurement, not an assertion.
TEST_CASE("STEP-008 measures the genuine large-file corpus when supplied",
          "[.][step-008-measure]")
{
    wchar_t* rawPath = nullptr;
    std::size_t rawLength = 0;
    if (_wdupenv_s(&rawPath, &rawLength, L"PREVIEW3D_MANUAL_STEP_FILE") != 0 || !rawPath
        || !*rawPath) {
        std::free(rawPath);
        WARN("PREVIEW3D_MANUAL_STEP_FILE is not set; skipping large-file measurement");
        return;
    }
    const std::wstring sourcePath(rawPath);
    std::free(rawPath);

    import_broker::ImportSessionRequest request;
    request.format = import_broker::ImportFormat::Step;
    request.sourcePath = sourcePath;
    request.stepHostExePath = PREVIEW3D_STEP_HOST_EXE;
    request.generationId = 0x6800;
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = import_broker::kImportMaxChunkCount;
    request.maxChunkBatchesPerGeneration = 4096;
    request.maxChunksPerGeneration = 0;
    request.replyTimeoutMs = 1'800'000;

    std::vector<model_core::StepProgressNotice> events;
    request.onStepProgress = [&events](const model_core::StepProgressNotice& notice) {
        events.push_back(notice);
    };
    std::uint64_t peakHostCommit = 0;
    request.cpuBudgetAllows = [&peakHostCommit](std::uint64_t workerPrivateBytes) {
        peakHostCommit = (std::max)(peakHostCommit, workerPrivateBytes);
        return true;
    };
    std::uint64_t batches = 0, chunks = 0, triangles = 0, vertices = 0;
    const auto start = std::chrono::steady_clock::now();
    double firstBatchMs = -1.0;
    request.onBatch = [&](std::vector<import_broker::ValidatedChunk>&& batch) {
        if (firstBatchMs < 0.0) {
            firstBatchMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
        }
        ++batches;
        for (const auto& chunk : batch) {
            ++chunks;
            triangles += chunk.descriptor.indexCount / 3;
            vertices += chunk.descriptor.vertexCount;
        }
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
    request.isCancelled = [&deadline] {
        return std::chrono::steady_clock::now() > deadline;
    };

    const auto result = import_broker::RunImportSession(request);
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    std::uint64_t preflight = 0, read = 0, transfer = 0, plan = 0, mesh = 0, emit = 0, hostTotal = 0;
    std::uint32_t definitions = 0;
    for (const auto& event : events) {
        switch (event.phase) {
        case model_core::kStepPhasePreflight: preflight = event.phaseMilliseconds; break;
        case model_core::kStepPhaseRead: read = event.phaseMilliseconds; break;
        case model_core::kStepPhaseTransfer: transfer = event.phaseMilliseconds; break;
        case model_core::kStepPhasePlan: plan = event.phaseMilliseconds; break;
        case model_core::kStepPhaseMesh: mesh += event.phaseMilliseconds; break;
        case model_core::kStepPhaseEmit: emit = event.phaseMilliseconds; break;
        default: break;
        }
        hostTotal = event.totalMilliseconds;
        definitions = event.definitionTotal;
    }
    std::printf("[step-008-measure] ok=%d code=%u preflight=%llu read=%llu "
                "transfer=%llu plan=%llu mesh=%llu emit=%llu hostTotal=%llu "
                "firstCoarse=%.0f brokerReady=%.0f "
                "definitions=%u batches=%llu chunks=%llu triangles=%llu vertices=%llu "
                "peakHostCommitBytes=%llu progressEvents=%zu\n",
                result.ok ? 1 : 0, uint32_t(result.errorCode),
                static_cast<unsigned long long>(preflight),
                static_cast<unsigned long long>(read),
                static_cast<unsigned long long>(transfer),
                static_cast<unsigned long long>(plan),
                static_cast<unsigned long long>(mesh),
                static_cast<unsigned long long>(emit),
                static_cast<unsigned long long>(hostTotal), firstBatchMs, elapsed, definitions,
                static_cast<unsigned long long>(batches),
                static_cast<unsigned long long>(chunks),
                static_cast<unsigned long long>(triangles),
                static_cast<unsigned long long>(vertices),
                static_cast<unsigned long long>(peakHostCommit), events.size());
    INFO("code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage));
    CHECK(result.ok);
}

// Opt-in regression for the post-STEP-008 viewer failure: the broker's default
// reply timeout is 120 s, but the host emits progress only at phase boundaries,
// so the genuine assembly's long XDE `Transfer` left the broker with no message
// long enough to declare the host wedged, surfacing as StepHostFailure. The
// host now emits a progress-driven Transfer heartbeat. This case runs the real
// corpus with a deliberately short reply timeout (below the phase-boundary gap
// but comfortably above the heartbeat interval) and asserts the session still
// completes.
//
//   set PREVIEW3D_MANUAL_STEP_FILE=<abs path to Voron_2.4r2_Assembly.step>
//   x64/Release/Tests.ImportIsolation.exe "[.][step-008-heartbeat]"
TEST_CASE("STEP-008 transfer heartbeat keeps a short broker reply timeout alive",
          "[.][step-008-heartbeat]")
{
    wchar_t* rawPath = nullptr;
    std::size_t rawLength = 0;
    if (_wdupenv_s(&rawPath, &rawLength, L"PREVIEW3D_MANUAL_STEP_FILE") != 0 || !rawPath
        || !*rawPath) {
        std::free(rawPath);
        WARN("PREVIEW3D_MANUAL_STEP_FILE is not set; skipping heartbeat regression");
        return;
    }
    const std::wstring sourcePath(rawPath);
    std::free(rawPath);

    import_broker::ImportSessionRequest request;
    request.format = import_broker::ImportFormat::Step;
    request.sourcePath = sourcePath;
    request.stepHostExePath = PREVIEW3D_STEP_HOST_EXE;
    request.generationId = 0x6810;
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = import_broker::kImportMaxChunkCount;
    request.maxChunkBatchesPerGeneration = 4096;
    request.maxChunksPerGeneration = 0;
    // Short enough that the pre-heartbeat Transfer gap (~64 s Release / ~445 s
    // Debug) would fail, but well above the 5 s heartbeat interval.
    request.replyTimeoutMs = 30'000;

    std::uint64_t transferEvents = 0;
    request.onStepProgress = [&transferEvents](const model_core::StepProgressNotice& notice) {
        if (notice.phase == model_core::kStepPhaseTransfer) ++transferEvents;
    };
    // The viewer always streams batches. Without this callback the broker
    // accumulates every payload against its 128 MiB no-callback cap and fails
    // the genuine assembly at ValidateSection for an unrelated reason.
    std::uint64_t chunks = 0;
    request.onBatch = [&chunks](std::vector<import_broker::ValidatedChunk>&& batch) {
        chunks += batch.size();
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
    request.isCancelled = [&deadline] {
        return std::chrono::steady_clock::now() > deadline;
    };

    const auto result = import_broker::RunImportSession(request);
    INFO("code " << uint32_t(result.errorCode) << " stage " << uint32_t(result.stage)
                 << " transferEvents " << transferEvents);
    CHECK(result.ok);
    // The phase-boundary event plus at least one throttled heartbeat.
    CHECK(transferEvents > 1);
}

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
#include <map>
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
        {"utf8-bom", std::string("\xef\xbb\xbf", 3) + Part21("#1=CARTESIAN_POINT('',(0.,0.,0.));"),
         StepPreflightStatus::Ok},
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
        for (const auto& chunk : batch) {
            if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList) {
                CHECK((chunk.descriptor.geometryFlags
                       & model_core::kGeometryReusableInstanceSource) != 0);
            }
        }
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
    std::uint32_t warnings = 0;
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
            if (chunk.descriptor.topology == model_core::ChunkTopology::ImportStatus
                && chunk.payload.size() >= sizeof(model_core::ImportStatusPayload)) {
                model_core::ImportStatusPayload status{};
                std::memcpy(&status, chunk.payload.data(), sizeof(status));
                warnings = status.optionalFeatureWarnings;
            }
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
                "peakHostCommitBytes=%llu progressEvents=%zu warnings=%u\n",
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
                static_cast<unsigned long long>(peakHostCommit), events.size(), warnings);
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

// Opt-in planner diagnostic for a locally supplied assembly. It streams the
// real host scene and reports node/instance/geometry totals, the number of
// distinct instance nodes, any two distinct geometry chunks carrying identical
// bytes (a duplicated definition), and any two same-definition instances whose
// world AABBs overlap by more than half the smaller volume (a duplicated or
// interfering placement), with the node ancestry of each overlapping pair.
// This is the tool used to triage the OQD_isolated_blade_trap_assembly.stp
// duplicate piece: that scene has no duplicate checksums and its overlapping
// pairs are distinct placements, so the host faithfully expands the OCCT XDE
// graph rather than duplicating an occurrence.
//
//   set PREVIEW3D_MANUAL_STEP_FILE=<abs path to a .stp/.step>
//   x64/Release/Tests.ImportIsolation.exe "[.][step-scene-diag]"
TEST_CASE("STEP planner scene diagnostic for a manual corpus item",
          "[.][step-scene-diag]")
{
    wchar_t* rawPath = nullptr;
    std::size_t rawLength = 0;
    if (_wdupenv_s(&rawPath, &rawLength, L"PREVIEW3D_MANUAL_STEP_FILE") != 0 || !rawPath
        || !*rawPath) {
        std::free(rawPath);
        WARN("PREVIEW3D_MANUAL_STEP_FILE is not set; skipping scene diagnostic");
        return;
    }
    const std::wstring sourcePath(rawPath);
    std::free(rawPath);

    import_broker::ImportSessionRequest request;
    request.format = import_broker::ImportFormat::Step;
    request.sourcePath = sourcePath;
    request.stepHostExePath = PREVIEW3D_STEP_HOST_EXE;
    request.generationId = 0x6820;
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = import_broker::kImportMaxChunkCount;
    request.maxChunkBatchesPerGeneration = 4096;
    request.maxChunksPerGeneration = 0;
    request.replyTimeoutMs = 1'800'000;
    request.cpuBudgetAllows = [](std::uint64_t) { return true; };

    std::vector<import_broker::ValidatedChunk> chunks;
    request.onBatch = [&chunks](std::vector<import_broker::ValidatedChunk>&& batch) {
        for (auto& chunk : batch) chunks.push_back(std::move(chunk));
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
    request.isCancelled = [&deadline] {
        return std::chrono::steady_clock::now() > deadline;
    };

    const auto result = import_broker::RunImportSession(request);
    REQUIRE(result.ok);

    std::uint32_t geometry = 0, materials = 0, nodes = 0, instances = 0, warnings = 0;
    std::uint32_t roots = 0;
    std::vector<std::uint64_t> geometryChecksums;
    std::map<std::uint32_t, std::uint32_t> meshIdByChunk;
    std::map<std::uint32_t, std::uint32_t> vertexByChunk;
    struct Placed {
        std::uint32_t geometryChunkId;
        std::uint32_t nodeId;
        double worldMin[3];
        double worldMax[3];
    };
    std::vector<Placed> placed;
    std::map<std::uint32_t, model_core::NodePayload> nodeById;
    for (const auto& chunk : chunks) {
        switch (chunk.descriptor.topology) {
        case model_core::ChunkTopology::TriangleList:
        case model_core::ChunkTopology::PointList:
            ++geometry;
            geometryChecksums.push_back(chunk.descriptor.chunkChecksum);
            meshIdByChunk[chunk.descriptor.chunkId] = chunk.descriptor.meshId;
            vertexByChunk[chunk.descriptor.chunkId] = chunk.descriptor.vertexCount;
            break;
        case model_core::ChunkTopology::Material: ++materials; break;
        case model_core::ChunkTopology::Node: {
            ++nodes;
            model_core::NodePayload payload{};
            if (chunk.payload.size() >= sizeof(payload)) {
                std::memcpy(&payload, chunk.payload.data(), sizeof(payload));
                nodeById[payload.nodeId] = payload;
                if (payload.parentNodeId == 0) ++roots;
            }
            break;
        }
        case model_core::ChunkTopology::MeshInstance: {
            ++instances;
            model_core::MeshInstancePayload payload{};
            if (chunk.payload.size() >= sizeof(payload)) {
                std::memcpy(&payload, chunk.payload.data(), sizeof(payload));
                Placed p{payload.geometryChunkId, payload.nodeId,
                         {payload.worldMin[0], payload.worldMin[1], payload.worldMin[2]},
                         {payload.worldMax[0], payload.worldMax[1], payload.worldMax[2]}};
                placed.push_back(p);
            }
            break;
        }
        case model_core::ChunkTopology::ImportStatus: {
            model_core::ImportStatusPayload status{};
            if (chunk.payload.size() >= sizeof(status)) {
                std::memcpy(&status, chunk.payload.data(), sizeof(status));
                warnings = status.optionalFeatureWarnings;
            }
            break;
        }
        default: break;
        }
    }

    std::sort(geometryChecksums.begin(), geometryChecksums.end());
    std::uint32_t duplicateChecksums = 0;
    for (std::size_t i = 1; i < geometryChecksums.size(); ++i)
        if (geometryChecksums[i] == geometryChecksums[i - 1]) ++duplicateChecksums;

    {
        std::vector<std::uint32_t> instanceNodes;
        instanceNodes.reserve(placed.size());
        for (const auto& p : placed) instanceNodes.push_back(p.nodeId);
        std::sort(instanceNodes.begin(), instanceNodes.end());
        const auto uniqueEnd = std::unique(instanceNodes.begin(), instanceNodes.end());
        std::printf("[step-scene-diag] distinctInstanceNodes=%zu\n",
                    static_cast<std::size_t>(uniqueEnd - instanceNodes.begin()));
    }

    // Two instances of the same definition whose world AABBs overlap by more
    // than half the smaller volume are a duplicate placement, not an assembly
    // of adjacent parts.
    const auto printChain = [&](const Placed& p) {
        std::uint32_t id = p.nodeId;
        int guard = 0;
        while (id != 0 && guard++ < 64) {
            const auto it = nodeById.find(id);
            if (it == nodeById.end()) break;
            std::printf("[step-scene-diag-chain] geom=%u node=%u parent=%u "
                        "t=(%.3f,%.3f,%.3f)\n",
                        p.geometryChunkId, id, it->second.parentNodeId,
                        it->second.localTransform[12], it->second.localTransform[13],
                        it->second.localTransform[14]);
            id = it->second.parentNodeId;
        }
    };
    std::uint32_t overlapping = 0;
    for (std::size_t i = 0; i < placed.size(); ++i) {
        for (std::size_t j = i + 1; j < placed.size(); ++j) {
            if (placed[i].geometryChunkId != placed[j].geometryChunkId) continue;
            double overlapVolume = 1.0, volumeI = 1.0, volumeJ = 1.0;
            for (int axis = 0; axis < 3; ++axis) {
                const double low = (std::max)(placed[i].worldMin[axis], placed[j].worldMin[axis]);
                const double high = (std::min)(placed[i].worldMax[axis], placed[j].worldMax[axis]);
                overlapVolume *= (std::max)(0.0, high - low);
                volumeI *= placed[i].worldMax[axis] - placed[i].worldMin[axis];
                volumeJ *= placed[j].worldMax[axis] - placed[j].worldMin[axis];
            }
            if (overlapVolume > 0.5 * (std::min)(volumeI, volumeJ)) {
                ++overlapping;
                std::printf("[step-scene-diag-overlap] geom=%u mesh=%u verts=%u "
                            "a=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f) "
                            "b=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f) overlap=%.2f\n",
                            placed[i].geometryChunkId,
                            meshIdByChunk[placed[i].geometryChunkId],
                            vertexByChunk[placed[i].geometryChunkId],
                            placed[i].worldMin[0], placed[i].worldMin[1], placed[i].worldMin[2],
                            placed[i].worldMax[0], placed[i].worldMax[1], placed[i].worldMax[2],
                            placed[j].worldMin[0], placed[j].worldMin[1], placed[j].worldMin[2],
                            placed[j].worldMax[0], placed[j].worldMax[1], placed[j].worldMax[2],
                            overlapVolume);
                printChain(placed[i]);
                printChain(placed[j]);
            }
        }
    }

    std::printf("[step-scene-diag] geometry=%u materials=%u nodes=%u roots=%u instances=%u "
                "meshCount=%u duplicateGeometryChecksums=%u overlappingSameDefinitionPairs=%u warnings=%u\n",
                geometry, materials, nodes, roots, instances,
                chunks.empty() ? 0u : chunks.front().scene.meshCount,
                duplicateChecksums, overlapping, warnings);
    {
        std::map<std::uint32_t, std::uint32_t> instancesByMesh;
        for (const auto& p : placed) {
            const auto it = meshIdByChunk.find(p.geometryChunkId);
            if (it != meshIdByChunk.end()) ++instancesByMesh[it->second];
        }
        for (const auto& [mesh, count] : instancesByMesh) {
            if (count > 1)
                std::printf("[step-scene-diag-mesh] mesh=%u instances=%u\n", mesh, count);
        }
    }
    CHECK(result.ok);
}

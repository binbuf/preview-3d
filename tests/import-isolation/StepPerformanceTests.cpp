#include <catch2/catch_test_macros.hpp>

#include "StepImporterVersion.h"
#include "StepPart21Preflight.h"
#include "StepTessellationProfile.h"

#include "import_broker/ImportSession.h"
#include "model_core/WireFormat.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

std::span<const std::byte> AsBytes(std::string_view text)
{
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// A syntactically valid Part-21 DATA section with `entities` entity records,
// used to prove the preflight's early entity-count abort without any OCCT work.
std::string Part21WithEntities(std::size_t entities)
{
    std::string text = "ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n";
    for (std::size_t i = 1; i <= entities; ++i) {
        text += "#" + std::to_string(i) + "=CARTESIAN_POINT('',(0.,0.,0.));\n";
    }
    text += "ENDSEC;\nEND-ISO-10303-21;\n";
    return text;
}

} // namespace

TEST_CASE("STEP-005 records the parallel meshing policy and delivery strategy",
          "[step-005][profile]")
{
    using namespace step_host;
    CHECK(kStepTessellationProfileVersion == 5);
    CHECK(kStepImporterVersion == 1 + kStepTessellationProfileVersion);

    const StepTessellationProfile profile;
    // STEP-005 corrected STEP-004's serial assumption: the pinned USE_TBB=OFF
    // port still exposes OSD_Parallel's built-in OSD_ThreadPool backend.
    CHECK(profile.parallel);
    CHECK(static_cast<uint32_t>(kStepDeliveryStrategy)
          == static_cast<uint32_t>(StepDeliveryStrategy::SinglePassProgressiveDisplay));
    CHECK(kStepDeliveryStrategyVersion >= 1);
}

TEST_CASE("STEP-005 parallel and forced-serial meshing emit identical normalized output",
          "[step-005][parallel][determinism]")
{
    auto parallel = Request(L"assembly_nested_ap214.stp", 0x6500);
    auto serial = Request(L"assembly_nested_ap214.stp", 0x6501);
    serial.stepForceSerialForTesting = true;

    const auto parallelResult = import_broker::RunImportSession(parallel);
    const auto serialResult = import_broker::RunImportSession(serial);
    INFO("parallel code " << uint32_t(parallelResult.errorCode)
                          << " serial code " << uint32_t(serialResult.errorCode));
    REQUIRE(parallelResult.ok);
    REQUIRE(serialResult.ok);
    CHECK(parallelResult.producer == import_broker::ImportProducer::StepHost);
    CHECK(serialResult.producer == import_broker::ImportProducer::StepHost);
    CHECK(parallelResult.batchCount == serialResult.batchCount);
    REQUIRE(parallelResult.chunks.size() == serialResult.chunks.size());
    for (std::size_t i = 0; i < parallelResult.chunks.size(); ++i) {
        CHECK(parallelResult.chunks[i].descriptor.chunkId == serialResult.chunks[i].descriptor.chunkId);
        CHECK(parallelResult.chunks[i].descriptor.byteSize == serialResult.chunks[i].descriptor.byteSize);
        CHECK(parallelResult.chunks[i].descriptor.chunkChecksum
              == serialResult.chunks[i].descriptor.chunkChecksum);
    }
}

TEST_CASE("STEP-005 publishes bounded ordered phase progress with timing evidence",
          "[step-005][progress]")
{
    std::vector<model_core::StepProgressNotice> events;
    auto request = Request(L"assembly_nested_ap214.stp", 0x6510);
    request.onStepProgress = [&events](const model_core::StepProgressNotice& notice) {
        events.push_back(notice);
    };
    const auto result = import_broker::RunImportSession(request);
    INFO("code " << uint32_t(result.errorCode) << " progress " << result.stepProgressCount);
    REQUIRE(result.ok);
    REQUIRE_FALSE(events.empty());
    CHECK(result.stepProgressCount == events.size());

    // The first event is admission; the ordered phases all appear.
    CHECK(events.front().phase == model_core::kStepPhasePreflight);
    const auto hasPhase = [&events](uint32_t phase) {
        return std::any_of(events.begin(), events.end(),
                           [phase](const model_core::StepProgressNotice& e) { return e.phase == phase; });
    };
    CHECK(hasPhase(model_core::kStepPhaseRead));
    CHECK(hasPhase(model_core::kStepPhaseTransfer));
    CHECK(hasPhase(model_core::kStepPhasePlan));
    CHECK(hasPhase(model_core::kStepPhaseMesh));
    CHECK(hasPhase(model_core::kStepPhaseEmit));

    // The lexed byte count is the real source size, proving admission covered
    // the whole file over the single mapped view.
    const auto fileSize = std::filesystem::file_size(StpFixture(L"assembly_nested_ap214.stp"));
    CHECK(events.front().preflightBytes == fileSize);

    // Mesh events are monotonic and finish at the definition total.
    uint32_t meshed = 0, total = 0;
    for (const auto& event : events) {
        if (event.phase != model_core::kStepPhaseMesh) continue;
        CHECK(event.definitionsMeshed >= meshed);
        CHECK(event.definitionTotal >= event.definitionsMeshed);
        meshed = event.definitionsMeshed;
        total = event.definitionTotal;
    }
    CHECK(total > 0);
    CHECK(meshed == total);

    // The terminal event is the emit phase and every event carries its own
    // generation and a bounded timing.
    CHECK(result.lastStepProgress.phase == model_core::kStepPhaseEmit);
    for (const auto& event : events) {
        CHECK(event.generationId == 0x6510);
        CHECK(event.reserved0 == 0);
        CHECK(event.totalMilliseconds >= event.phaseMilliseconds);
    }
}

TEST_CASE("STEP-005 admission aborts early on the entity cap without a full scan",
          "[step-005][envelope]")
{
    // Just under the (reduced) cap passes; just over fails at the cap and
    // reports lexed bytes far below the whole input, so an over-cap file never
    // pays for a full scan before it is typed.
    step_host::StepPreflightLimits limits;
    limits.maxEntityRecords = 256;

    const std::string under = Part21WithEntities(256);
    const std::string over = Part21WithEntities(4096);

    const auto accepted = step_host::StepPreflightBytes(AsBytes(under), limits);
    CHECK(accepted.ok());
    CHECK(accepted.entityRecords == 256);

    const auto rejected = step_host::StepPreflightBytes(AsBytes(over), limits);
    CHECK(rejected.status == step_host::StepPreflightStatus::EntityLimit);
    CHECK(rejected.entityRecords == 257);
    CHECK(rejected.lexedBytes < over.size() / 2);
}

// Opt-in measurement harness. Run explicitly with:
//   Tests.ImportIsolation.exe "[step-005-measure]"
// It prints the where-the-time-goes split for the small STEP-003 fixtures so
// the numbers can be recorded in STEP-005-VERIFICATION.md. It is hidden from
// the ordinary [step-005] selector because timing output is not an assertion.
TEST_CASE("STEP-005 measures the phase split on the fixture corpus",
          "[.][step-005-measure]")
{
    struct Row { const wchar_t* fixture; uint64_t generation; };
    const Row rows[] = {
        {L"part_ap203.stp", 0x6520},
        {L"assembly_ap214.stp", 0x6521},
        {L"assembly_nested_ap214.stp", 0x6522},
    };
    for (const Row& row : rows) {
        std::vector<model_core::StepProgressNotice> events;
        auto request = Request(row.fixture, row.generation);
        request.onStepProgress = [&events](const model_core::StepProgressNotice& notice) {
            events.push_back(notice);
        };
        const auto result = import_broker::RunImportSession(request);
        REQUIRE(result.ok);
        uint64_t preflight = 0, read = 0, transfer = 0, plan = 0, mesh = 0, emit = 0, total = 0;
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
            total = event.totalMilliseconds;
        }
        std::printf("[step-005-measure] %ls bytes=%llu preflight=%llu read=%llu transfer=%llu "
                    "plan=%llu mesh=%llu emit=%llu total=%llu defs=%u chunks=%u batches=%u\n",
                    row.fixture,
                    static_cast<unsigned long long>(std::filesystem::file_size(StpFixture(row.fixture))),
                    static_cast<unsigned long long>(preflight),
                    static_cast<unsigned long long>(read),
                    static_cast<unsigned long long>(transfer),
                    static_cast<unsigned long long>(plan),
                    static_cast<unsigned long long>(mesh),
                    static_cast<unsigned long long>(emit),
                    static_cast<unsigned long long>(total),
                    result.lastStepProgress.definitionTotal,
                    result.chunks.empty() ? 0u : static_cast<unsigned>(result.chunks.size()),
                    result.batchCount);
    }
}

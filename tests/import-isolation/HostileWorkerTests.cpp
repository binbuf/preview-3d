// Gate 2 workstream A, part 3: the synthetic hostile-worker suite. Proves
// the host's copy-then-validate acceptance path (shared/import-broker/
// SharedSectionValidator.h) actually rejects a worker that lies, rather
// than merely trusting a well-behaved one -- per the Gate 2 exit criteria
// in .docs/design/10-delivery-plan.md: "the synthetic hostile-worker build
// is rejected by the host's validator on every injected fault... with no
// corrupted bytes reaching the upload path and no elevation beyond the
// sandbox's zero-capability token."
//
// Three of the four named faults are covered here (mutation after the
// host's first read, stale-generation replay, and two layout/offset lie
// variants), each launched through the exact same AppContainer + Job Object
// sandbox as the honest worker. The fourth (overrunning Job Object limits)
// is already proven by SandboxLaunchTests.cpp's --overallocate/--hang tests
// against the identical sandboxed launch mechanism -- not duplicated here.

#include "GenerationLaunchSupport.h"
#include "SandboxTestSupport.h"
#include "import_broker/SandboxLauncher.h"
#include "import_broker/SharedSection.h"
#include "import_broker/SharedSectionValidator.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "platform/MappedView.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

namespace {

std::optional<generation_launch_support::GenerationLaunch> LaunchHostileWorker(
    const platform::AppContainerSid& sid, const std::wstring& mode, HANDLE sectionHandle)
{
    return generation_launch_support::LaunchWorkerWithControlChannel(
        sandbox_test_support::HostileWorkerExePath(), mode, sid, sectionHandle);
}

} // namespace

TEST_CASE("Hostile worker mutating shared-section bytes after ChunksReady does not corrupt what "
          "the host already validated and copied",
          "[hostile-worker]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto section = import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);
    REQUIRE(section);

    auto launch = LaunchHostileWorker(fixture.sid, L"--mutate-after-ready", section.get());
    REQUIRE(launch.has_value());
    REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));

    constexpr uint64_t kGenerationId = 400;
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

    // Validate immediately -- strictly before the worker's own 300ms
    // corruption delay even starts.
    auto view = platform::MappedView::Map(section.get(), FILE_MAP_READ,
                                           import_broker::kSyntheticSectionBytes);
    REQUIRE(view);
    auto result = import_broker::ValidateAndCopySection(view.bytes(), kGenerationId, 8);
    REQUIRE(result.ok);
    REQUIRE(result.chunks.size() == 2);

    model_core::VertexPositionNormalUv0F32 vertex0{};
    std::memcpy(&vertex0, result.chunks[0].payload.data(), sizeof(vertex0));
    CHECK(vertex0.px == -0.5f);

    // Waiting for process exit is a genuine Win32 happens-before guarantee
    // that the worker's corrupting write (on its main thread, before
    // returning from main()) has already occurred.
    DWORD waitResult = WaitForSingleObject(launch->proc.process.get(), 5000);
    REQUIRE(waitResult == WAIT_OBJECT_0);

    // Confirm the attack was real: the raw section now differs from what
    // was validated.
    auto rawView = platform::MappedView::Map(section.get(), FILE_MAP_READ,
                                              import_broker::kSyntheticSectionBytes);
    REQUIRE(rawView);
    float rawPx = 0.0f;
    uint64_t chunk0Offset = model_core::kSectionHeaderSize + 2 * model_core::kChunkDescriptorSize;
    std::memcpy(&rawPx, rawView.bytes().data() + chunk0Offset, sizeof(rawPx));
    CHECK(rawPx != -0.5f);

    // The host's already-copied private data is untouched by the later
    // mutation -- re-read straight from the validated result, not the
    // earlier local copy above.
    model_core::VertexPositionNormalUv0F32 vertex0AfterMutation{};
    std::memcpy(&vertex0AfterMutation, result.chunks[0].payload.data(), sizeof(vertex0AfterMutation));
    CHECK(vertex0AfterMutation.px == -0.5f);
}

TEST_CASE("Hostile worker replaying a stale generation ID inside the section is rejected as an "
          "ImportProtocolViolation",
          "[hostile-worker]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto section = import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);
    REQUIRE(section);

    auto launch = LaunchHostileWorker(fixture.sid, L"--replay-generation", section.get());
    REQUIRE(launch.has_value());
    REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));

    constexpr uint64_t kGenerationId = 500;
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

    auto view = platform::MappedView::Map(section.get(), FILE_MAP_READ,
                                           import_broker::kSyntheticSectionBytes);
    REQUIRE(view);
    auto result = import_broker::ValidateAndCopySection(view.bytes(), kGenerationId, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::ImportProtocolViolation);

    WaitForSingleObject(launch->proc.process.get(), 5000);
}

TEST_CASE("Hostile worker claiming a chunk payload range past sectionLength is rejected as "
          "MalformedData",
          "[hostile-worker]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto section = import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);
    REQUIRE(section);

    auto launch = LaunchHostileWorker(fixture.sid, L"--lie-offset", section.get());
    REQUIRE(launch.has_value());
    REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));

    constexpr uint64_t kGenerationId = 600;
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

    auto view = platform::MappedView::Map(section.get(), FILE_MAP_READ,
                                           import_broker::kSyntheticSectionBytes);
    REQUIRE(view);
    auto result = import_broker::ValidateAndCopySection(view.bytes(), kGenerationId, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    CHECK(result.chunks.empty());

    WaitForSingleObject(launch->proc.process.get(), 5000);
}

TEST_CASE("Hostile worker claiming an unrecognized vertex layout ID is rejected as MalformedData",
          "[hostile-worker]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto section = import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);
    REQUIRE(section);

    auto launch = LaunchHostileWorker(fixture.sid, L"--lie-layout", section.get());
    REQUIRE(launch.has_value());
    REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));

    constexpr uint64_t kGenerationId = 700;
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

    auto view = platform::MappedView::Map(section.get(), FILE_MAP_READ,
                                           import_broker::kSyntheticSectionBytes);
    REQUIRE(view);
    auto result = import_broker::ValidateAndCopySection(view.bytes(), kGenerationId, 8);
    CHECK_FALSE(result.ok);
    CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    CHECK(result.chunks.empty());

    WaitForSingleObject(launch->proc.process.get(), 5000);
}

TEST_CASE("Hostile scene-instance graph attacks are rejected before any record is published",
          "[hostile-worker][fbx-002]")
{
    const std::wstring modes[]={L"--scene-invalid-parent",L"--scene-cycle",L"--scene-nan",L"--scene-inf",
        L"--scene-illegal-topology",L"--scene-duplicate",L"--scene-oversized",L"--scene-unresolved"};
    uint64_t generation=800;
    for(size_t modeIndex=0;modeIndex<std::size(modes);++modeIndex){const auto& mode=modes[modeIndex];DYNAMIC_SECTION("scene attack " << modeIndex){
        sandbox_test_support::SandboxFixture fixture;
        auto section=import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);REQUIRE(section);
        auto launch=LaunchHostileWorker(fixture.sid,mode,section.get());REQUIRE(launch);REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));
        model_core::StartGenerationRequest request{};request.generationId=generation++;request.sceneVariant=model_core::kSceneVariant_CubeAndPointCluster;
        request.sectionHandleValue=reinterpret_cast<uint64_t>(section.get());request.sectionByteCapacity=import_broker::kSyntheticSectionBytes;request.maxChunkCount=8;
        REQUIRE(model_core::WriteControlMessage(launch->controlInWrite.get(),model_core::ControlOpcode::StartGeneration,&request,sizeof(request)));
        auto received=model_core::ReadControlMessage(launch->controlOutRead.get());REQUIRE(received);
        auto view=platform::MappedView::Map(section.get(),FILE_MAP_READ,import_broker::kSyntheticSectionBytes);REQUIRE(view);
        auto result=import_broker::ValidateAndCopySection(view.bytes(),request.generationId,request.maxChunkCount);
        CHECK_FALSE(result.ok);CHECK(result.chunks.empty());WaitForSingleObject(launch->proc.process.get(),5000);
    }}
}

TEST_CASE("Hostile worker cannot mutate an accepted instance hierarchy after the host copy",
          "[hostile-worker][fbx-002]")
{
    sandbox_test_support::SandboxFixture fixture;
    auto section=import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);REQUIRE(section);
    auto launch=LaunchHostileWorker(fixture.sid,L"--scene-mutate",section.get());REQUIRE(launch);REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));
    model_core::StartGenerationRequest request{};request.generationId=900;request.sceneVariant=model_core::kSceneVariant_CubeAndPointCluster;
    request.sectionHandleValue=reinterpret_cast<uint64_t>(section.get());request.sectionByteCapacity=import_broker::kSyntheticSectionBytes;request.maxChunkCount=8;
    REQUIRE(model_core::WriteControlMessage(launch->controlInWrite.get(),model_core::ControlOpcode::StartGeneration,&request,sizeof(request)));
    auto received=model_core::ReadControlMessage(launch->controlOutRead.get());REQUIRE(received);
    auto view=platform::MappedView::Map(section.get(),FILE_MAP_READ,import_broker::kSyntheticSectionBytes);REQUIRE(view);
    auto result=import_broker::ValidateAndCopySection(view.bytes(),request.generationId,8);REQUIRE(result.ok);REQUIRE(result.chunks.size()==4);
    model_core::NodePayload accepted{};std::memcpy(&accepted,result.chunks[2].payload.data(),sizeof(accepted));CHECK(accepted.localTransform[12]==2);
    REQUIRE(WaitForSingleObject(launch->proc.process.get(),5000)==WAIT_OBJECT_0);
    model_core::NodePayload after{};std::memcpy(&after,result.chunks[2].payload.data(),sizeof(after));CHECK(after.localTransform[12]==2);
}

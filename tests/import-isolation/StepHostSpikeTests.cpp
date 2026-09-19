#include <catch2/catch_test_macros.hpp>

#include "../../interactive-viewer/src/app/D3D12ImportBridge.h"
#include "import_broker/ImportSession.h"
#include "import_broker/SandboxLauncher.h"
#include "model_core/ImportError.h"
#include "model_core/WireFormat.h"
#include "platform/AppContainerSid.h"
#include "platform/Win32Handle.h"

#include <windows.h>

#include <chrono>
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

std::wstring StepHostPath() { return PREVIEW3D_STEP_HOST_EXE; }

// Committed STEP-003 fixtures. The broker opens the path itself and duplicates
// an already-open read-only handle to the sandboxed host; the test never hands
// a path into the container.
std::wstring StpFixture(const wchar_t* name)
{
    return std::wstring(PREVIEW3D_STP_FIXTURES_DIR) + name;
}

std::wstring StepHostDirectory()
{
    const std::wstring exe = StepHostPath();
    const auto slash = exe.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : exe.substr(0, slash);
}

// Writes bytes to a unique temp file and returns its path. The broker opens it
// itself; the test never hands a path to the sandboxed host.
std::wstring WriteTempSource(std::string_view bytes, const wchar_t* suffix)
{
    wchar_t directory[MAX_PATH]{};
    REQUIRE(GetTempPathW(MAX_PATH, directory) != 0);
    const std::wstring path = std::wstring(directory) + L"preview3d-step-"
        + std::to_wstring(GetCurrentProcessId()) + L"-"
        + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count())
        + suffix;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output);
    if (!bytes.empty()) output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    REQUIRE(output.good());
    return path;
}

import_broker::ImportSessionRequest BaseRequest(const std::wstring& sourcePath,
                                                uint64_t generationId)
{
    import_broker::ImportSessionRequest request;
    request.format = import_broker::ImportFormat::Step;
    request.sourcePath = sourcePath;
    request.stepHostExePath = StepHostPath();
    request.generationId = generationId;
    // The product transfer window and chunk catalog caps (import_broker::
    // kImportSectionBytes / kImportMaxChunkCount). A real STEP assembly scene
    // emits nodes, reusable geometry, materials, and instances in one section.
    request.sectionByteCapacity = 64ull * 1024 * 1024;
    request.maxChunkCount = 1024;
    request.maxChunksPerGeneration = 1024;
    request.maxChunkBatchesPerGeneration = 1;
    request.replyTimeoutMs = 15'000;
    return request;
}

struct TempFileGuard {
    std::wstring path;
    ~TempFileGuard() { if (!path.empty()) { std::error_code error; std::filesystem::remove(path, error); } }
};

struct HostProfile {
    std::wstring name;
    platform::AppContainerSid sid;
    std::vector<std::byte> sidCopy;

    HostProfile()
        : name(L"Binbuf.Preview3D.StepHost.Test-" + std::to_wstring(GetCurrentProcessId()) + L"-"
               + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()))
        , sid(platform::AppContainerSid::CreateOrOpen(name, L"Preview3D STEP Host Test",
                                                       L"STEP-002 zero-capability step host"))
    {
        REQUIRE(sid);
        sidCopy.resize(GetLengthSid(sid.get()));
        REQUIRE(CopySid(static_cast<DWORD>(sidCopy.size()), sidCopy.data(), sid.get()));
        REQUIRE(platform::GrantDirectoryReadExecute(StepHostDirectory(), sid.get()));
    }

    ~HostProfile()
    {
        if (!sidCopy.empty()) platform::RevokeDirectoryAccess(StepHostDirectory(), sidCopy.data());
        platform::AppContainerSid::Delete(name);
    }
};

std::string RunAuthorityProbe(const HostProfile& profile, const std::wstring& canary)
{
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE inReadRaw{}, inWriteRaw{}, outReadRaw{}, outWriteRaw{};
    REQUIRE(CreatePipe(&inReadRaw, &inWriteRaw, &attributes, 0));
    REQUIRE(CreatePipe(&outReadRaw, &outWriteRaw, &attributes, 0));
    platform::Win32Handle inRead(inReadRaw), inWrite(inWriteRaw), outRead(outReadRaw), outWrite(outWriteRaw);
    SetHandleInformation(inWrite.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0);
    std::vector<HANDLE> inherited{inRead.get(), outWrite.get()};
    const std::wstring executable = StepHostPath();
    const std::wstring arguments = L"\"" + executable + L"\" --probes \"" + canary + L"\" 9";
    import_broker::SandboxLimits limits{};
    auto process = import_broker::LaunchSuspendedSandboxed(executable, arguments, inherited,
        outWrite.get(), limits, profile.sid, inRead.get());
    REQUIRE(process);
    REQUIRE(import_broker::ResumeSandboxProcess(*process));
    inRead.reset();
    outWrite.reset();
    WaitForSingleObject(process->process.get(), 15'000);
    std::string output;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(outRead.get(), nullptr, 0, nullptr, &available, nullptr) || !available) break;
        char buffer[256]{};
        DWORD read = 0;
        if (!ReadFile(outRead.get(), buffer, sizeof(buffer), &read, nullptr) || !read) break;
        output.append(buffer, read);
    }
    TerminateJobObject(process->job.get(), 0);
    return output;
}

} // namespace

TEST_CASE("STEP-002 host imports a valid Part-21 file through the dedicated route",
          "[step-002][step-host][sandbox]")
{
    const std::wstring source = StpFixture(L"part_ap214.stp");
    auto request = BaseRequest(source, 0x5701);
    const auto result = import_broker::RunImportSession(request);
    INFO("stage " << uint32_t(result.stage) << " code " << uint32_t(result.errorCode)
                  << " openErrorBytes " << result.openError.size());
    REQUIRE(result.ok);
    CHECK(result.producer == import_broker::ImportProducer::StepHost);
    REQUIRE_FALSE(result.chunks.empty());
    CHECK(result.chunks.front().scene.format == model_core::SourceFormatId::Step);
    CHECK(result.chunks.front().scene.upAxis == model_core::UpAxisId::Unknown);
    CHECK(result.chunks.front().scene.metersPerUnit > 0.0);
    bool haveGeometry = false;
    for (const auto& chunk : result.chunks) {
        haveGeometry = haveGeometry
            || chunk.descriptor.topology == model_core::ChunkTopology::TriangleList
            || chunk.descriptor.topology == model_core::ChunkTopology::PointList;
    }
    CHECK(haveGeometry);
}

TEST_CASE("STEP-002 viewer bridge routes an explicit Step format and extension discovery",
          "[step-002][viewer-bridge]")
{
    // STEP-007 exposes the extension, but the byte admission below still runs
    // in the dedicated host before any CAD-kernel work.
    CHECK(d3d12_import_bridge::ClassifyByExtension(L"part.step")
          == d3d12_import_bridge::SourceFormat::Step);
    CHECK(d3d12_import_bridge::ClassifyByExtension(L"part.stp")
          == d3d12_import_bridge::SourceFormat::Step);

    const std::wstring source = StpFixture(L"part_ap214.stp");
    const auto result = d3d12_import_bridge::RunImport(
        d3d12_import_bridge::SourceFormat::Step, source, 0x5720);
    INFO("stage " << uint32_t(result.errorStage) << " code " << uint32_t(result.errorCode));
    REQUIRE(result.ok);
    CHECK(result.scene.format == model_core::SourceFormatId::Step);
    CHECK_FALSE(result.meshes.empty());
}

TEST_CASE("STEP-002 host rejects malformed and external Part-21 before OCCT transfer",
          "[step-002][step-host][admission][negative]")
{
    SECTION("non Part-21 bytes") {
        TempFileGuard source{WriteTempSource("this is not a STEP file", L".stp")};
        const auto result = import_broker::RunImportSession(BaseRequest(source.path, 0x5702));
        CHECK_FALSE(result.ok);
        CHECK(result.producer == import_broker::ImportProducer::StepHost);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }
    SECTION("required external document") {
        constexpr std::string_view external =
            "ISO-10303-21;\nHEADER;\n"
            "FILE_POPULATION(('AP214'),('x'),($),$);\n"
            "ENDSEC;\nDATA;\n#1=APPLICATION_CONTEXT('core data');\nENDSEC;\n"
            "END-ISO-10303-21;\n";
        TempFileGuard source{WriteTempSource(external, L".stp")};
        const auto result = import_broker::RunImportSession(BaseRequest(source.path, 0x5703));
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature);
    }
}

TEST_CASE("STEP-002 host observes cancellation", "[step-002][step-host][cancel]")
{
    TempFileGuard source{WriteTempSource(kMinimalPart21, L".stp")};
    auto request = BaseRequest(source.path, 0x5704);
    request.isCancelled = [] { return true; };
    const auto result = import_broker::RunImportSession(request);
    CHECK_FALSE(result.ok);
    CHECK(result.stage == import_broker::ImportStage::Cancelled);
    CHECK(result.errorCode == model_core::ImportErrorCode::Cancelled);
}

TEST_CASE("STEP-002 host faults are typed and a later import recovers",
          "[step-002][step-host][recovery]")
{
    struct Fault { const wchar_t* flag; const char* label; model_core::ImportErrorCode expected; };
    const Fault faults[] = {
        {L"--pool-crash", "crash", model_core::ImportErrorCode::StepHostFailure},
        {L"--pool-hang", "hang", model_core::ImportErrorCode::StepHostFailure},
        {L"--pool-stale", "stale", model_core::ImportErrorCode::StepHostFailure},
        {L"--pool-wrong-format", "wrong-format", model_core::ImportErrorCode::StepHostFailure},
        {L"--pool-unknown-error", "unknown-error", model_core::ImportErrorCode::StepHostFailure},
        {L"--pool-overallocate", "overallocate", model_core::ImportErrorCode::StepHostLimit},
    };
    uint64_t generation = 0x5710;
    for (const auto& fault : faults) {
        const std::wstring source = StpFixture(L"assembly_ap214.stp");
        auto request = BaseRequest(source, generation++);
        request.stepHostArgumentsOverride = fault.flag;
        if (std::wstring_view(fault.flag) == L"--pool-overallocate")
            request.stepHostCommitLimitBytes = 64ull * 1024 * 1024;
        if (std::wstring_view(fault.flag) == L"--pool-hang")
            request.replyTimeoutMs = 750;
        const auto result = import_broker::RunImportSession(request);
        INFO("fault " << fault.label << " code " << uint32_t(result.errorCode)
                      << " stage " << uint32_t(result.stage));
        CHECK_FALSE(result.ok);
        CHECK(result.producer == import_broker::ImportProducer::StepHost);
        CHECK(result.errorCode == fault.expected);

        // Same-process recovery: the next valid generation must succeed without
        // any viewer restart.
        const std::wstring recovered = StpFixture(L"part_ap214.stp");
        const auto after = import_broker::RunImportSession(BaseRequest(recovered, generation++));
        CHECK(after.ok);
    }
}

TEST_CASE("STEP-002 host denies path network and child-process authority",
          "[step-002][step-host][containment]")
{
    HostProfile profile;
    TempFileGuard canary{WriteTempSource(kMinimalPart21, L".stp")};
    const std::string output = RunAuthorityProbe(profile, canary.path);
    INFO(output);
    CHECK(output.find("FS_PROBE=DENIED") != std::string::npos);
    CHECK(output.find("NET_PROBE=DENIED") != std::string::npos);
    CHECK(output.find("SPAWN_PROBE=DENIED") != std::string::npos);
    CHECK(output.find("UNEXPECTED_SUCCESS") == std::string::npos);
}

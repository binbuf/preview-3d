// Stage 3 (the live sidecar-request protocol) tests.
//
// A pure unit test proves ServiceSidecarRequest's decode/rejection logic
// without any process at all (GetCurrentProcess() as a stand-in target --
// duplication succeeds trivially against your own process, so this proves
// the *decoding*/*resolution* wiring, not cross-process duplication).
//
// The real, previously-unverified risk this stage's plan flagged --
// whether SandboxProcess::process (the host's own handle to a child it just
// launched) carries PROCESS_DUP_HANDLE rights -- is resolved empirically by
// the second test below, launching a real AppContainer worker and
// duplicating into it directly, exactly as production code does.

#include "GenerationLaunchSupport.h"
#include "SandboxTestSupport.h"
#include "import_broker/SandboxLauncher.h"
#include "import_broker/SharedSection.h"
#include "import_broker/SidecarRequestServicer.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <fstream>
#include <string>

namespace {

model_core::RequestSidecarFileNotice MakeRequest(const std::string& relativePath, uint64_t generationId = 1)
{
    model_core::RequestSidecarFileNotice request{};
    request.generationId = generationId;
    request.relativePathLength = static_cast<uint32_t>(relativePath.size());
    std::memcpy(request.relativePathUtf8, relativePath.data(), relativePath.size());
    return request;
}

} // namespace

TEST_CASE("ServiceSidecarRequest replies Ready for a valid relative sidecar reference", "[sidecar-protocol]")
{
    wchar_t tempDir[MAX_PATH]{};
    REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
    std::wstring directory = std::wstring(tempDir) + L"p3d_sidecar_proto_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(directory.c_str(), nullptr);
    std::wstring primaryPath = directory + L"\\scene.gltf";
    { std::ofstream primary(primaryPath, std::ios::binary | std::ios::trunc); primary << "{}"; }
    std::wstring sidecarPath = directory + L"\\mesh.bin";
    { std::ofstream sidecar(sidecarPath, std::ios::binary | std::ios::trunc); sidecar << "abcd"; }

    HANDLE rawPrimary = CreateFileW(primaryPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(rawPrimary != INVALID_HANDLE_VALUE);
    wchar_t canonicalBuf[MAX_PATH * 2]{};
    DWORD canonicalLen = GetFinalPathNameByHandleW(rawPrimary, canonicalBuf, MAX_PATH * 2, FILE_NAME_NORMALIZED);
    REQUIRE(canonicalLen != 0);
    std::wstring primaryCanonicalPath(canonicalBuf, canonicalLen);
    CloseHandle(rawPrimary);

    auto request = MakeRequest("mesh.bin");
    auto serviced = import_broker::ServiceSidecarRequest(GetCurrentProcess(), primaryCanonicalPath, request,
                                                           /*maxSidecarFileBytes=*/1024);
    REQUIRE(std::holds_alternative<model_core::SidecarFileReadyNotice>(serviced));
    const auto& ready = std::get<model_core::SidecarFileReadyNotice>(serviced);
    CHECK(ready.sidecarByteLength == 4);
    CHECK(ready.sidecarFileHandleValue != 0);
    CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ready.sidecarFileHandleValue)));

    DeleteFileW(sidecarPath.c_str());
    DeleteFileW(primaryPath.c_str());
    RemoveDirectoryW(directory.c_str());
}

TEST_CASE("ServiceSidecarRequest replies Unavailable for a path-traversal attempt", "[sidecar-protocol]")
{
    wchar_t tempDir[MAX_PATH]{};
    REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
    std::wstring directory = std::wstring(tempDir) + L"p3d_sidecar_proto2_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(directory.c_str(), nullptr);
    std::wstring primaryPath = directory + L"\\scene.gltf";
    { std::ofstream primary(primaryPath, std::ios::binary | std::ios::trunc); primary << "{}"; }

    HANDLE rawPrimary = CreateFileW(primaryPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(rawPrimary != INVALID_HANDLE_VALUE);
    wchar_t canonicalBuf[MAX_PATH * 2]{};
    DWORD canonicalLen = GetFinalPathNameByHandleW(rawPrimary, canonicalBuf, MAX_PATH * 2, FILE_NAME_NORMALIZED);
    REQUIRE(canonicalLen != 0);
    std::wstring primaryCanonicalPath(canonicalBuf, canonicalLen);
    CloseHandle(rawPrimary);

    auto request = MakeRequest("../../../../Windows/win.ini");
    auto serviced = import_broker::ServiceSidecarRequest(GetCurrentProcess(), primaryCanonicalPath, request, 1024);
    REQUIRE(std::holds_alternative<model_core::SidecarFileUnavailableNotice>(serviced));
    const auto& unavailable = std::get<model_core::SidecarFileUnavailableNotice>(serviced);
    CHECK(unavailable.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::UnsafeReference));

    DeleteFileW(primaryPath.c_str());
    RemoveDirectoryW(directory.c_str());
}

TEST_CASE("An oversized declared relativePathLength is rejected without reading past the buffer",
          "[sidecar-protocol]")
{
    model_core::RequestSidecarFileNotice request{};
    request.generationId = 1;
    request.relativePathLength = model_core::kMaxSidecarRelativePathBytes + 1; // declared bigger than the buffer

    auto serviced = import_broker::ServiceSidecarRequest(GetCurrentProcess(), L"C:\\nowhere\\scene.gltf", request, 1024);
    REQUIRE(std::holds_alternative<model_core::SidecarFileUnavailableNotice>(serviced));
    CHECK(std::get<model_core::SidecarFileUnavailableNotice>(serviced).errorCode
          == static_cast<uint32_t>(model_core::ImportErrorCode::UnsafeReference));
}

TEST_CASE("A real AppContainer-sandboxed worker process can receive a mid-generation duplicated handle "
          "(resolves the PROCESS_DUP_HANDLE risk flagged for this stage)",
          "[sidecar-protocol]")
{
    sandbox_test_support::SandboxFixture fixture;

    wchar_t tempDir[MAX_PATH]{};
    REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
    std::wstring directory = std::wstring(tempDir) + L"p3d_sidecar_proto3_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(directory.c_str(), nullptr);
    std::wstring primaryPath = directory + L"\\scene.gltf";
    { std::ofstream primary(primaryPath, std::ios::binary | std::ios::trunc); primary << "{}"; }
    std::wstring sidecarPath = directory + L"\\mesh.bin";
    { std::ofstream sidecar(sidecarPath, std::ios::binary | std::ios::trunc); sidecar << "abcd"; }

    HANDLE rawPrimary = CreateFileW(primaryPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(rawPrimary != INVALID_HANDLE_VALUE);
    wchar_t canonicalBuf[MAX_PATH * 2]{};
    DWORD canonicalLen = GetFinalPathNameByHandleW(rawPrimary, canonicalBuf, MAX_PATH * 2, FILE_NAME_NORMALIZED);
    REQUIRE(canonicalLen != 0);
    std::wstring primaryCanonicalPath(canonicalBuf, canonicalLen);
    CloseHandle(rawPrimary);

    auto section = import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);
    REQUIRE(section);
    auto launch = generation_launch_support::LaunchWorkerWithControlChannel(
        sandbox_test_support::WorkerExePath(), L"--child-noop", fixture.sid, section.get());
    REQUIRE(launch.has_value());
    // Deliberately never resumed -- DuplicateHandle's PROCESS_DUP_HANDLE
    // check is against the handle's access rights from CreateProcessW,
    // independent of whether the process has actually started running.

    auto request = MakeRequest("mesh.bin");
    auto serviced = import_broker::ServiceSidecarRequest(launch->proc.process.get(), primaryCanonicalPath, request,
                                                           1024);
    REQUIRE(std::holds_alternative<model_core::SidecarFileReadyNotice>(serviced));
    CHECK(std::get<model_core::SidecarFileReadyNotice>(serviced).sidecarFileHandleValue != 0);

    DeleteFileW(sidecarPath.c_str());
    DeleteFileW(primaryPath.c_str());
    RemoveDirectoryW(directory.c_str());
}

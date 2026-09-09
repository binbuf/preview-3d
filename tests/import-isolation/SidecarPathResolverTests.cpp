// Stage 2 (sidecar path-containment validator) unit tests -- pure, no
// sandbox/worker needed. Exercises import_broker::ResolveSidecarPath
// directly against a real temp-directory tree.

#include "import_broker/SidecarPathResolver.h"
#include "import_broker/SourceFileAccess.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <fstream>
#include <string>

namespace {

// A fresh scratch directory per test, containing a primary file
// ("scene.gltf") and whatever sidecar files the test writes into it (or
// outside it, to exercise escape rejection). Cleaned up on destruction.
struct ScratchGltfDirectory {
    std::wstring directory;
    std::wstring primaryCanonicalPath;

    ScratchGltfDirectory()
    {
        wchar_t tempDir[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
        directory = std::wstring(tempDir) + L"p3d_sidecar_" + std::to_wstring(GetCurrentProcessId()) + L"_"
            + std::to_wstring(reinterpret_cast<uintptr_t>(this));
        REQUIRE(CreateDirectoryW(directory.c_str(), nullptr));

        std::wstring primaryPath = directory + L"\\scene.gltf";
        std::ofstream primary(primaryPath, std::ios::binary | std::ios::trunc);
        primary << "{}";
        primary.close();

        auto opened = import_broker::OpenAndCanonicalizeSourceFile(primaryPath);
        REQUIRE(opened.file);
        primaryCanonicalPath = opened.canonicalPath;
    }

    void WriteSibling(const std::wstring& relativeName, size_t sizeBytes = 4) const
    {
        std::ofstream out(directory + L"\\" + relativeName, std::ios::binary | std::ios::trunc);
        std::string content(sizeBytes, 'x');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    ~ScratchGltfDirectory()
    {
        DeleteFileW((directory + L"\\scene.gltf").c_str());
        DeleteFileW((directory + L"\\mesh.bin").c_str());
        DeleteFileW((directory + L"\\diffuse.png").c_str());
        RemoveDirectoryW(directory.c_str());
    }
};

} // namespace

TEST_CASE("A valid relative sidecar reference resolves successfully", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    dir.WriteSibling(L"mesh.bin");

    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "mesh.bin", 1024);
    REQUIRE(result.file);
    CHECK(result.fileSizeBytes == 4);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::None);
}

TEST_CASE("An empty sidecar reference is rejected", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("A drive-relative reference (contains ':') is rejected", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "C:mesh.bin", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("A data: URI scheme reference is rejected by the ':' rule", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "data:application/octet-stream;base64,AAAA", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("A UNC-style reference is rejected", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "\\\\localhost\\share\\mesh.bin", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("An absolute reference is rejected", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "/etc/mesh.bin", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("A '..'-traversal reference is rejected", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "../outside.bin", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
}

TEST_CASE("A reference with a disallowed extension is rejected", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    dir.WriteSibling(L"payload.exe");
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "payload.exe", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::UnsafeReference);
    DeleteFileW((dir.directory + L"\\payload.exe").c_str());
}

TEST_CASE("A missing sidecar file is rejected as FileUnavailable", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "missing.bin", 1024);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::FileUnavailable);
}

TEST_CASE("A sidecar file exceeding the byte cap is rejected as FileUnavailable", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    dir.WriteSibling(L"mesh.bin", 100);
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "mesh.bin", /*maxSidecarFileBytes=*/10);
    CHECK_FALSE(result.file);
    CHECK(result.rejectionCode == model_core::ImportErrorCode::FileUnavailable);
}

TEST_CASE(".ktx2 is an allowed sidecar extension", "[sidecar-resolver]")
{
    ScratchGltfDirectory dir;
    dir.WriteSibling(L"texture.ktx2");
    auto result = import_broker::ResolveSidecarPath(dir.primaryCanonicalPath, "texture.ktx2", 1024);
    CHECK(result.file);
    DeleteFileW((dir.directory + L"\\texture.ktx2").c_str());
}

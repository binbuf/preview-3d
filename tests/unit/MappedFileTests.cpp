// MappedFile/MappingLease primitive (.docs/design/03-file-formats-and-
// ingestion.md, "Mapped-file abstraction"). Proven standalone against
// real on-disk .glb test fixtures via a locally-opened handle -- the same
// "prove it in isolation before a real caller touches it" discipline
// already used for every D3D12 graphics slice. FromHandle() -- the
// constructor path a duplicated-handle caller like the sandboxed worker
// actually uses -- is now wired into the real import pipeline; see
// tests/import-isolation/GltfImportTests.cpp's RunGltfImportFromRealFile
// and SourceFileAccessTests.cpp for the trusted-process-side open/
// canonicalize/duplicate steps around it.

#include <model_core/MappedFile.h>

#include <platform/Win32Handle.h>

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

std::wstring TestAssetPath(const wchar_t* fileName)
{
    return std::wstring(PREVIEW3D_TEST_ASSETS_DIR) + fileName;
}

std::vector<std::byte> ReadWholeFile(const std::wstring& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    REQUIRE(in.good());
    std::streamsize size = in.tellg();
    REQUIRE(size > 0);
    in.seekg(0, std::ios::beg);

    std::vector<std::byte> bytes(static_cast<size_t>(size));
    REQUIRE(in.read(reinterpret_cast<char*>(bytes.data()), size));
    return bytes;
}

// A deterministic-content scratch file larger than one allocation-
// granularity unit, so MapWindow's internal alignment/padding logic can
// be exercised against a real boundary -- none of the checked-in .glb
// fixtures are anywhere near big enough for that on their own.
struct ScratchFile {
    std::wstring path;

    explicit ScratchFile(size_t sizeBytes)
    {
        wchar_t tempDir[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
        wchar_t tempPath[MAX_PATH]{};
        REQUIRE(GetTempFileNameW(tempDir, L"pf3", 0, tempPath) != 0);
        path = tempPath;

        std::vector<std::byte> pattern(sizeBytes);
        for (size_t i = 0; i < sizeBytes; ++i) {
            pattern[i] = static_cast<std::byte>(i % 256);
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out.write(reinterpret_cast<const char*>(pattern.data()), static_cast<std::streamsize>(pattern.size()));
        REQUIRE(out.good());
    }

    ~ScratchFile() { DeleteFileW(path.c_str()); }

    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;
};

} // namespace

TEST_CASE("Open succeeds against a real fixture and reports the correct size", "[model_core]")
{
    std::wstring path = TestAssetPath(L"tri_tight.glb");

    auto result = model_core::MappedFile::Open(path);
    REQUIRE(result.file.has_value());
    CHECK(result.error.empty());

    auto expectedSize = std::filesystem::file_size(path);
    CHECK(result.file->SizeBytes() == expectedSize);
}

TEST_CASE("Open on a nonexistent path fails cleanly", "[model_core]")
{
    auto result = model_core::MappedFile::Open(TestAssetPath(L"does_not_exist.glb"));
    CHECK_FALSE(result.file.has_value());
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("MapWhole returns byte-exact content against an independently-read reference", "[model_core]")
{
    std::wstring path = TestAssetPath(L"tri_tight.glb");
    std::vector<std::byte> expected = ReadWholeFile(path);

    auto openResult = model_core::MappedFile::Open(path);
    REQUIRE(openResult.file.has_value());

    std::wstring error;
    auto lease = openResult.file->MapWhole(error);
    REQUIRE(static_cast<bool>(lease));
    CHECK(error.empty());

    auto bytes = lease.Bytes();
    REQUIRE(bytes.size() == expected.size());
    CHECK(std::equal(bytes.begin(), bytes.end(), expected.begin()));
}

TEST_CASE("MapWindow returns exactly the requested sub-range of a real fixture", "[model_core]")
{
    std::wstring path = TestAssetPath(L"tri_interleaved.glb");
    std::vector<std::byte> expected = ReadWholeFile(path);
    REQUIRE(expected.size() > 32); // fixture must be big enough for a meaningful sub-range

    auto openResult = model_core::MappedFile::Open(path);
    REQUIRE(openResult.file.has_value());

    uint64_t offset = 8;
    uint64_t length = 16;
    std::wstring error;
    auto lease = openResult.file->MapWindow(offset, length, error);
    REQUIRE(static_cast<bool>(lease));
    CHECK(error.empty());

    auto bytes = lease.Bytes();
    REQUIRE(bytes.size() == length);
    CHECK(std::equal(bytes.begin(), bytes.end(), expected.begin() + static_cast<ptrdiff_t>(offset)));
}

TEST_CASE("MapWindow crossing a real allocation-granularity boundary is byte-exact", "[model_core]")
{
    constexpr size_t kScratchSize = 200 * 1024; // comfortably more than one 64 KiB granularity unit
    ScratchFile scratch(kScratchSize);

    auto openResult = model_core::MappedFile::Open(scratch.path);
    REQUIRE(openResult.file.has_value());
    REQUIRE(openResult.file->SizeBytes() == kScratchSize);

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    uint64_t granularity = info.dwAllocationGranularity;
    REQUIRE(granularity > 0);

    // Deliberately not aligned to `granularity` -- proves MapWindow's
    // internal alignment-and-hide-the-padding logic, not just a
    // pass-through to MapViewOfFile (which would fail outright on a
    // misaligned offset).
    uint64_t offset = granularity + granularity / 3;
    uint64_t length = 4096;
    REQUIRE(offset + length <= kScratchSize);

    std::wstring error;
    auto lease = openResult.file->MapWindow(offset, length, error);
    REQUIRE(static_cast<bool>(lease));
    CHECK(error.empty());

    auto bytes = lease.Bytes();
    REQUIRE(bytes.size() == length);
    for (uint64_t i = 0; i < length; ++i) {
        CHECK(bytes[i] == static_cast<std::byte>((offset + i) % 256));
    }
}

TEST_CASE("MapWindow rejects a range past the end of the file", "[model_core]")
{
    std::wstring path = TestAssetPath(L"tri_tight.glb");
    auto openResult = model_core::MappedFile::Open(path);
    REQUIRE(openResult.file.has_value());

    std::wstring error;
    auto lease = openResult.file->MapWindow(0, openResult.file->SizeBytes() + 1, error);
    CHECK_FALSE(static_cast<bool>(lease));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Two MappingLeases from the same MappedFile are simultaneously valid and correct", "[model_core]")
{
    std::wstring path = TestAssetPath(L"tri_tight.glb");
    std::vector<std::byte> expected = ReadWholeFile(path);
    REQUIRE(expected.size() >= 16);

    auto openResult = model_core::MappedFile::Open(path);
    REQUIRE(openResult.file.has_value());

    std::wstring errorA;
    std::wstring errorB;
    auto leaseA = openResult.file->MapWindow(0, 8, errorA);
    auto leaseB = openResult.file->MapWindow(8, 8, errorB);
    REQUIRE(static_cast<bool>(leaseA));
    REQUIRE(static_cast<bool>(leaseB));

    CHECK(std::equal(leaseA.Bytes().begin(), leaseA.Bytes().end(), expected.begin()));
    CHECK(std::equal(leaseB.Bytes().begin(), leaseB.Bytes().end(), expected.begin() + 8));
}

TEST_CASE("FromHandle on an already-open handle produces the same identity and content as Open", "[model_core]")
{
    std::wstring path = TestAssetPath(L"tri_tight.glb");
    std::vector<std::byte> expected = ReadWholeFile(path);

    auto viaOpen = model_core::MappedFile::Open(path);
    REQUIRE(viaOpen.file.has_value());

    // A bare CreateFileW here, deliberately not going through
    // MappedFile::Open or import_broker::OpenAndCanonicalizeSourceFile --
    // this test only cares that FromHandle correctly builds a MappedFile
    // from *some* already-open handle, matching what a worker receives
    // after handle duplication (proven separately in
    // tests/import-isolation/SourceFileAccessTests.cpp).
    HANDLE rawFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(rawFile != INVALID_HANDLE_VALUE);

    auto viaHandle = model_core::MappedFile::FromHandle(platform::Win32Handle(rawFile));
    REQUIRE(viaHandle.file.has_value());

    CHECK(viaHandle.file->Identity() == viaOpen.file->Identity());
    CHECK(viaHandle.file->SizeBytes() == expected.size());

    std::wstring error;
    auto lease = viaHandle.file->MapWhole(error);
    REQUIRE(static_cast<bool>(lease));
    CHECK(error.empty());

    auto bytes = lease.Bytes();
    REQUIRE(bytes.size() == expected.size());
    CHECK(std::equal(bytes.begin(), bytes.end(), expected.begin()));
}

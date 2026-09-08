// Gate 2's "crash-safe bounded derived-cache prototype with hit/miss/
// corruption/eviction injection" deliverable. See DerivedCache.h's own
// header comment for the two deliberate scoping deltas from the full
// design-doc cache (identity source, opaque payload shape).

#include "DerivedCache.h"

#include <platform/Win32Handle.h>

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace {

model_core::FileIdentity MakeIdentity(uint64_t volumeSerial, uint64_t sizeBytes)
{
    model_core::FileIdentity identity;
    identity.volumeSerialNumber = volumeSerial;
    identity.fileId128.fill(std::byte{ 0xAB });
    identity.sizeBytes = sizeBytes;
    return identity;
}

// A scratch cache directory, uniquely named under the system temp path,
// recursively cleaned up on destruction -- mirrors MappedFileTests.cpp's
// ScratchFile RAII pattern, extended for a directory of entries rather
// than a single file.
struct ScratchCacheDir {
    std::wstring path;

    ScratchCacheDir()
    {
        wchar_t tempDir[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
        wchar_t unique[MAX_PATH]{};
        REQUIRE(GetTempFileNameW(tempDir, L"dc", 0, unique) != 0);
        // GetTempFileNameW creates a FILE at `unique` to reserve the name;
        // delete it and reuse the generated unique name as a directory.
        DeleteFileW(unique);
        path = unique;
        REQUIRE(CreateDirectoryW(path.c_str(), nullptr));
    }

    ~ScratchCacheDir()
    {
        WIN32_FIND_DATAW findData{};
        std::wstring pattern = path + L"\\*";
        HANDLE find = FindFirstFileW(pattern.c_str(), &findData);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                std::wstring name = findData.cFileName;
                if (name == L"." || name == L"..") {
                    continue;
                }
                DeleteFileW((path + L"\\" + name).c_str());
            } while (FindNextFileW(find, &findData));
            FindClose(find);
        }
        RemoveDirectoryW(path.c_str());
    }

    ScratchCacheDir(const ScratchCacheDir&) = delete;
    ScratchCacheDir& operator=(const ScratchCacheDir&) = delete;
};

std::wstring FindSingleCacheFile(const std::wstring& directory)
{
    WIN32_FIND_DATAW findData{};
    std::wstring pattern = directory + L"\\*.cache";
    HANDLE find = FindFirstFileW(pattern.c_str(), &findData);
    REQUIRE(find != INVALID_HANDLE_VALUE);
    std::wstring path = directory + L"\\" + findData.cFileName;
    FindClose(find);
    return path;
}

void FlipByteInFile(const std::wstring& path, LONG offset)
{
    platform::Win32Handle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    REQUIRE(file);
    REQUIRE(SetFilePointer(file.get(), offset, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER);

    std::byte value{};
    DWORD readBytes = 0;
    REQUIRE(ReadFile(file.get(), &value, 1, &readBytes, nullptr));
    REQUIRE(readBytes == 1);
    value = static_cast<std::byte>(static_cast<unsigned char>(value) ^ 0xFF);

    REQUIRE(SetFilePointer(file.get(), offset, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER);
    DWORD written = 0;
    REQUIRE(WriteFile(file.get(), &value, 1, &written, nullptr));
}

} // namespace

TEST_CASE("Put then TryGet round-trips byte-exact", "[cache]")
{
    ScratchCacheDir dir;
    std::wstring error;
    auto cache = DerivedCache::Open(dir.path, {}, error);
    REQUIRE(cache.has_value());

    CacheEntryKey key;
    key.identity = MakeIdentity(111, 500);
    key.parserVersion = 1;

    std::vector<std::byte> payload(64);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i);
    }

    REQUIRE(cache->Put(key, payload, error));

    auto result = cache->TryGet(key);
    REQUIRE(result.has_value());
    CHECK(*result == payload);
}

TEST_CASE("TryGet misses cleanly on an absent key", "[cache]")
{
    ScratchCacheDir dir;
    std::wstring error;
    auto cache = DerivedCache::Open(dir.path, {}, error);
    REQUIRE(cache.has_value());

    CacheEntryKey key;
    key.identity = MakeIdentity(1, 1);
    CHECK_FALSE(cache->TryGet(key).has_value());
}

TEST_CASE("A corrupted entry is rejected as a miss, not accepted", "[cache]")
{
    ScratchCacheDir dir;
    std::wstring error;
    auto cache = DerivedCache::Open(dir.path, {}, error);
    REQUIRE(cache.has_value());

    CacheEntryKey key;
    key.identity = MakeIdentity(2, 2);
    std::vector<std::byte> payload(32, std::byte{ 0x42 });
    REQUIRE(cache->Put(key, payload, error));
    REQUIRE(cache->TryGet(key).has_value()); // sanity: valid before corruption

    std::wstring path = FindSingleCacheFile(dir.path);
    FlipByteInFile(path, /*offset=*/88 + 5); // 88 = sizeof(CacheEntryHeader); inside the payload region

    CHECK_FALSE(cache->TryGet(key).has_value());
}

TEST_CASE("A truncated entry is rejected as a miss", "[cache]")
{
    ScratchCacheDir dir;
    std::wstring error;
    auto cache = DerivedCache::Open(dir.path, {}, error);
    REQUIRE(cache.has_value());

    CacheEntryKey key;
    key.identity = MakeIdentity(3, 3);
    std::vector<std::byte> payload(64, std::byte{ 0x11 });
    REQUIRE(cache->Put(key, payload, error));

    std::wstring path = FindSingleCacheFile(dir.path);
    {
        platform::Win32Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                                FILE_ATTRIBUTE_NORMAL, nullptr));
        REQUIRE(file);
        REQUIRE(SetFilePointer(file.get(), 50, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER);
        REQUIRE(SetEndOfFile(file.get()));
    }

    CHECK_FALSE(cache->TryGet(key).has_value());
}

TEST_CASE("A wrong-schema-version entry is rejected as a miss", "[cache]")
{
    ScratchCacheDir dir;
    std::wstring error;
    auto cache = DerivedCache::Open(dir.path, {}, error);
    REQUIRE(cache.has_value());

    CacheEntryKey key;
    key.identity = MakeIdentity(4, 4);
    std::vector<std::byte> payload(16, std::byte{ 0x77 });
    REQUIRE(cache->Put(key, payload, error));

    std::wstring path = FindSingleCacheFile(dir.path);
    {
        // schemaVersion is the second uint32_t field, at byte offset 4.
        platform::Win32Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                                FILE_ATTRIBUTE_NORMAL, nullptr));
        REQUIRE(file);
        REQUIRE(SetFilePointer(file.get(), 4, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER);
        uint32_t badVersion = 9999;
        DWORD written = 0;
        REQUIRE(WriteFile(file.get(), &badVersion, sizeof(badVersion), &written, nullptr));
    }

    CHECK_FALSE(cache->TryGet(key).has_value());
}

TEST_CASE("A Put exceeding the per-entry cap is rejected cleanly", "[cache]")
{
    ScratchCacheDir dir;
    DerivedCache::Options options;
    options.maxEntryBytes = 100;
    std::wstring error;
    auto cache = DerivedCache::Open(dir.path, options, error);
    REQUIRE(cache.has_value());

    CacheEntryKey key;
    key.identity = MakeIdentity(5, 5);
    std::vector<std::byte> tooLarge(200, std::byte{ 0x01 });

    CHECK_FALSE(cache->Put(key, tooLarge, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(cache->TryGet(key).has_value());
}

TEST_CASE("Writing past the total cap evicts the oldest entry first and stays under cap", "[cache]")
{
    ScratchCacheDir dir;
    DerivedCache::Options options;
    options.maxEntryBytes = 1024;
    options.maxTotalBytes = 400; // fits exactly 2 of the 188-byte entries below, not 3
    std::wstring error;
    auto cache = DerivedCache::Open(dir.path, options, error);
    REQUIRE(cache.has_value());

    std::vector<std::byte> payload(100, std::byte{ 0x09 }); // + 88-byte header = 188 bytes/entry on disk

    CacheEntryKey key1;
    key1.identity = MakeIdentity(10, 1);
    key1.parserVersion = 1;
    CacheEntryKey key2;
    key2.identity = MakeIdentity(10, 2);
    key2.parserVersion = 1;
    CacheEntryKey key3;
    key3.identity = MakeIdentity(10, 3);
    key3.parserVersion = 1;

    REQUIRE(cache->Put(key1, payload, error));
    // A short sleep between writes makes last-write-time ordering
    // deterministic rather than relying on filesystem timestamp
    // resolution being finer than the time these calls actually take.
    Sleep(20);
    REQUIRE(cache->Put(key2, payload, error));
    Sleep(20);
    REQUIRE(cache->Put(key3, payload, error));

    CHECK(cache->CurrentTotalBytes() <= options.maxTotalBytes);
    CHECK_FALSE(cache->TryGet(key1).has_value()); // oldest -- evicted
    CHECK(cache->TryGet(key2).has_value());
    CHECK(cache->TryGet(key3).has_value());
}

TEST_CASE("A stray .tmp file left behind is cleaned up on the next Open", "[cache]")
{
    ScratchCacheDir dir;
    std::wstring strayPath = dir.path + L"\\deadbeef.cache.tmp";
    {
        platform::Win32Handle file(
            CreateFileW(strayPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        REQUIRE(file);
    }
    REQUIRE(GetFileAttributesW(strayPath.c_str()) != INVALID_FILE_ATTRIBUTES);

    std::wstring error;
    auto cache = DerivedCache::Open(dir.path, {}, error);
    REQUIRE(cache.has_value());

    CHECK(GetFileAttributesW(strayPath.c_str()) == INVALID_FILE_ATTRIBUTES);
}

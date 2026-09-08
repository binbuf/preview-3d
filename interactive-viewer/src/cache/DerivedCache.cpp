#include "DerivedCache.h"

#include <platform/Sha256.h>
#include <platform/Win32Handle.h>

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr uint32_t kCacheEntryMagic = 0x43443350; // "P3DC"
constexpr uint32_t kCurrentCacheSchemaVersion = 1;

#pragma pack(push, 1)
struct CacheEntryHeader
{
    uint32_t magic;
    uint32_t schemaVersion;
    uint64_t volumeSerialNumber;
    uint8_t fileId128[16];
    uint64_t sourceSizeBytes;
    uint32_t parserVersion;
    uint32_t reserved0;
    uint64_t payloadLength;
    uint8_t payloadSha256[32];
};
static_assert(sizeof(CacheEntryHeader) == 88, "CacheEntryHeader layout changed");
#pragma pack(pop)

std::wstring HexEncode(std::span<const std::byte> bytes)
{
    static const wchar_t kHexDigits[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(bytes.size() * 2);
    for (std::byte b : bytes) {
        auto value = static_cast<unsigned char>(b);
        result.push_back(kHexDigits[value >> 4]);
        result.push_back(kHexDigits[value & 0x0F]);
    }
    return result;
}

void AppendBytes(std::vector<std::byte>& out, const void* data, size_t size)
{
    const auto* bytePtr = static_cast<const std::byte*>(data);
    out.insert(out.end(), bytePtr, bytePtr + size);
}

// Deletes every non-directory file in `directory` matching `pattern`
// (e.g. L"*.tmp", L"*.cache"), optionally invoking `beforeDelete` with each
// full path and its WIN32_FIND_DATAW first (used by EvictUntilFits to
// gather candidates instead of deleting immediately).
template <typename Callback>
void ForEachMatchingFile(const std::wstring& directory, const wchar_t* pattern, Callback&& callback)
{
    std::wstring searchPattern = directory + L"\\" + pattern;
    WIN32_FIND_DATAW findData{};
    HANDLE find = FindFirstFileW(searchPattern.c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        callback(directory + L"\\" + findData.cFileName, findData);
    } while (FindNextFileW(find, &findData));
    FindClose(find);
}

} // namespace

std::optional<DerivedCache> DerivedCache::Open(std::wstring directory, const Options& options,
                                                std::wstring& error)
{
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = L"The cache directory could not be created.";
        return std::nullopt;
    }

    ForEachMatchingFile(directory, L"*.tmp", [](const std::wstring& path, const WIN32_FIND_DATAW&) {
        DeleteFileW(path.c_str());
    });

    return DerivedCache(std::move(directory), options);
}

std::wstring DerivedCache::EntryPath(const CacheEntryKey& key) const
{
    std::vector<std::byte> packed;
    packed.reserve(sizeof(key.identity.volumeSerialNumber) + key.identity.fileId128.size()
                    + sizeof(key.identity.sizeBytes) + sizeof(key.parserVersion));
    AppendBytes(packed, &key.identity.volumeSerialNumber, sizeof(key.identity.volumeSerialNumber));
    AppendBytes(packed, key.identity.fileId128.data(), key.identity.fileId128.size());
    AppendBytes(packed, &key.identity.sizeBytes, sizeof(key.identity.sizeBytes));
    AppendBytes(packed, &key.parserVersion, sizeof(key.parserVersion));

    auto digest = platform::ComputeSha256(packed);
    // ComputeSha256 failing here would indicate a real environment
    // problem (the OS's own SHA-256 provider unavailable) -- "invalid" is
    // just a safe fallback filename, not expected to ever be reached.
    std::wstring fileName = digest ? HexEncode(*digest) : L"invalid";
    return directory_ + L"\\" + fileName + L".cache";
}

std::optional<std::vector<std::byte>> DerivedCache::TryGet(const CacheEntryKey& key) const
{
    std::wstring path = EntryPath(key);
    platform::Win32Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return std::nullopt; // absent = miss
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < static_cast<LONGLONG>(sizeof(CacheEntryHeader))) {
        return std::nullopt;
    }
    if (static_cast<uint64_t>(size.QuadPart) > sizeof(CacheEntryHeader) + options_.maxEntryBytes) {
        return std::nullopt; // absurdly oversized declared entry -- never a huge read
    }

    std::vector<std::byte> raw(static_cast<size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    if (!ReadFile(file.get(), raw.data(), static_cast<DWORD>(raw.size()), &bytesRead, nullptr)
        || bytesRead != raw.size()) {
        return std::nullopt;
    }

    CacheEntryHeader header{};
    std::memcpy(&header, raw.data(), sizeof(header));

    if (header.magic != kCacheEntryMagic || header.schemaVersion != kCurrentCacheSchemaVersion) {
        return std::nullopt;
    }
    if (header.volumeSerialNumber != key.identity.volumeSerialNumber) {
        return std::nullopt;
    }
    if (std::memcmp(header.fileId128, key.identity.fileId128.data(), sizeof(header.fileId128)) != 0) {
        return std::nullopt;
    }
    if (header.sourceSizeBytes != key.identity.sizeBytes || header.parserVersion != key.parserVersion) {
        return std::nullopt;
    }

    uint64_t expectedTotal = sizeof(CacheEntryHeader) + header.payloadLength;
    if (expectedTotal != raw.size()) {
        return std::nullopt; // truncated, or the declared length lied
    }

    std::span<const std::byte> payload(raw.data() + sizeof(CacheEntryHeader),
                                        static_cast<size_t>(header.payloadLength));
    auto actualDigest = platform::ComputeSha256(payload);
    if (!actualDigest || std::memcmp(actualDigest->data(), header.payloadSha256, actualDigest->size()) != 0) {
        return std::nullopt; // corrupted payload
    }

    return std::vector<std::byte>(payload.begin(), payload.end());
}

bool DerivedCache::Put(const CacheEntryKey& key, std::span<const std::byte> payload, std::wstring& error)
{
    if (payload.size() > options_.maxEntryBytes) {
        error = L"The entry exceeds the configured per-entry size cap.";
        return false;
    }

    auto digest = platform::ComputeSha256(payload);
    if (!digest) {
        error = L"The payload could not be hashed.";
        return false;
    }

    CacheEntryHeader header{};
    header.magic = kCacheEntryMagic;
    header.schemaVersion = kCurrentCacheSchemaVersion;
    header.volumeSerialNumber = key.identity.volumeSerialNumber;
    std::memcpy(header.fileId128, key.identity.fileId128.data(), sizeof(header.fileId128));
    header.sourceSizeBytes = key.identity.sizeBytes;
    header.parserVersion = key.parserVersion;
    header.payloadLength = payload.size();
    std::memcpy(header.payloadSha256, digest->data(), digest->size());

    EvictUntilFits(sizeof(header) + payload.size());

    std::wstring finalPath = EntryPath(key);
    std::wstring tempPath = finalPath + L".tmp";

    bool ok;
    {
        platform::Win32Handle file(CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                                FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) {
            error = L"The temporary cache file could not be created.";
            return false;
        }

        DWORD written = 0;
        ok = WriteFile(file.get(), &header, sizeof(header), &written, nullptr) && written == sizeof(header);
        if (ok && !payload.empty()) {
            ok = WriteFile(file.get(), payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr)
                && written == payload.size();
        }
        if (ok) {
            ok = FlushFileBuffers(file.get()) != 0;
        }
    } // file closed before rename

    if (!ok) {
        DeleteFileW(tempPath.c_str());
        error = L"Writing the cache entry failed.";
        return false;
    }

    if (!MoveFileExW(tempPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tempPath.c_str());
        error = L"The cache entry could not be committed.";
        return false;
    }
    return true;
}

void DerivedCache::EvictUntilFits(uint64_t incomingBytes)
{
    for (;;) {
        struct Candidate
        {
            std::wstring path;
            uint64_t size;
            FILETIME lastWrite;
        };
        std::vector<Candidate> candidates;
        uint64_t total = 0;

        ForEachMatchingFile(directory_, L"*.cache", [&](const std::wstring& path, const WIN32_FIND_DATAW& data) {
            uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            total += size;
            candidates.push_back(Candidate{ path, size, data.ftLastWriteTime });
        });

        if (total + incomingBytes <= options_.maxTotalBytes || candidates.empty()) {
            break;
        }

        auto oldest = std::min_element(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return CompareFileTime(&a.lastWrite, &b.lastWrite) < 0;
        });
        DeleteFileW(oldest->path.c_str());
    }
}

void DerivedCache::Clear()
{
    ForEachMatchingFile(directory_, L"*.cache", [](const std::wstring& path, const WIN32_FIND_DATAW&) {
        DeleteFileW(path.c_str());
    });
}

uint64_t DerivedCache::CurrentTotalBytes() const
{
    uint64_t total = 0;
    ForEachMatchingFile(directory_, L"*.cache", [&total](const std::wstring&, const WIN32_FIND_DATAW& data) {
        total += (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    });
    return total;
}

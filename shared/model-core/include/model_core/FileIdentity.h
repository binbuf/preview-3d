#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace model_core
{
struct FileIdentity
{
    uint64_t volumeSerialNumber = 0;
    std::array<std::byte, 16> fileId128{}; // FILE_ID_INFO::FileId, from GetFileInformationByHandleEx
    uint64_t sizeBytes = 0;
    int64_t lastWriteTime = 0;

    bool operator==(const FileIdentity& other) const noexcept
    {
        return volumeSerialNumber == other.volumeSerialNumber && fileId128 == other.fileId128 &&
               sizeBytes == other.sizeBytes && lastWriteTime == other.lastWriteTime;
    }
    bool operator!=(const FileIdentity& other) const noexcept
    {
        return !(*this == other);
    }
};
} // namespace model_core

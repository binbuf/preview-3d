#include "model_core/MappedFile.h"

#include <platform/CheckedMath.h>

#include <windows.h>

#include <cstring>

namespace model_core {

namespace {

uint64_t AllocationGranularity()
{
    static const uint64_t granularity = [] {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        return static_cast<uint64_t>(info.dwAllocationGranularity);
    }();
    return granularity;
}

DWORD AccessHintFlags(MappedFileAccessHint hint)
{
    switch (hint) {
    case MappedFileAccessHint::Sequential:
        return FILE_FLAG_SEQUENTIAL_SCAN;
    case MappedFileAccessHint::Random:
        return FILE_FLAG_RANDOM_ACCESS;
    case MappedFileAccessHint::Unknown:
    default:
        return 0;
    }
}

} // namespace

MappedFileOpenResult MappedFile::Open(const std::wstring& path, MappedFileAccessHint hint)
{
    MappedFileOpenResult result;

    HANDLE rawFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | AccessHintFlags(hint), nullptr);
    if (rawFile == INVALID_HANDLE_VALUE) {
        result.error = L"The file could not be opened (Windows error " + std::to_wstring(GetLastError()) + L").";
        return result;
    }

    return BuildFromOpenFile(platform::Win32Handle(rawFile));
}

MappedFileOpenResult MappedFile::FromHandle(platform::Win32Handle file)
{
    return BuildFromOpenFile(std::move(file));
}

MappedFileOpenResult MappedFile::BuildFromOpenFile(platform::Win32Handle file)
{
    MappedFileOpenResult result;

    FILE_STANDARD_INFO standardInfo{};
    if (!GetFileInformationByHandleEx(file.get(), FileStandardInfo, &standardInfo, sizeof(standardInfo))) {
        result.error = L"The file's standard information could not be read.";
        return result;
    }
    if (standardInfo.Directory) {
        result.error = L"The selected item is not a regular file.";
        return result;
    }
    uint64_t sizeBytes = static_cast<uint64_t>(standardInfo.EndOfFile.QuadPart);
    if (sizeBytes == 0) {
        result.error = L"The file is empty.";
        return result;
    }

    FILE_ID_INFO idInfo{};
    if (!GetFileInformationByHandleEx(file.get(), FileIdInfo, &idInfo, sizeof(idInfo))) {
        result.error = L"The file's identity could not be read.";
        return result;
    }

    FileIdentity identity;
    identity.volumeSerialNumber = idInfo.VolumeSerialNumber;
    std::memcpy(identity.fileId128.data(), idInfo.FileId.Identifier, identity.fileId128.size());
    identity.sizeBytes = sizeBytes;

    HANDLE rawMapping = CreateFileMappingW(file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (rawMapping == nullptr) {
        result.error = L"A read-only mapping of the file could not be created.";
        return result;
    }
    platform::Win32Handle mapping(rawMapping);

    result.file = MappedFile(std::move(file), std::move(mapping), identity);
    return result;
}

MappingLease MappedFile::MapWindow(uint64_t offset, uint64_t length, std::wstring& error) const
{
    if (length == 0) {
        error = L"A mapping window's length must be non-zero.";
        return MappingLease();
    }

    auto endOpt = platform::CheckedAdd(offset, length);
    if (!endOpt || *endOpt > identity_.sizeBytes) {
        error = L"The requested mapping window is out of bounds.";
        return MappingLease();
    }

    uint64_t granularity = AllocationGranularity();
    uint64_t alignedOffset = (offset / granularity) * granularity;
    uint64_t paddingBefore = offset - alignedOffset;
    uint64_t alignedLength = *endOpt - alignedOffset;

    platform::MappedView view = platform::MappedView::Map(mapping_.get(), FILE_MAP_READ,
                                                            static_cast<SIZE_T>(alignedLength), alignedOffset);
    if (!view) {
        error =
            L"The requested window could not be mapped (Windows error " + std::to_wstring(GetLastError()) + L").";
        return MappingLease();
    }

    return MappingLease(std::move(view), paddingBefore, length);
}

MappingLease MappedFile::MapWhole(std::wstring& error) const
{
    return MapWindow(0, identity_.sizeBytes, error);
}

} // namespace model_core

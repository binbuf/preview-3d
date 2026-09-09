#include "import_broker/SourceFileAccess.h"

#include <utility>

namespace import_broker {

OpenSourceFileResult OpenAndCanonicalizeSourceFile(const std::wstring& path)
{
    OpenSourceFileResult result;

    HANDLE rawFile =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                    nullptr);
    if (rawFile == INVALID_HANDLE_VALUE) {
        result.error = L"The file could not be opened (Windows error " + std::to_wstring(GetLastError()) + L").";
        return result;
    }
    platform::Win32Handle file(rawFile);

    DWORD requiredLength = GetFinalPathNameByHandleW(file.get(), nullptr, 0, FILE_NAME_NORMALIZED);
    if (requiredLength == 0) {
        result.error = L"The file's canonical path could not be determined (Windows error "
            + std::to_wstring(GetLastError()) + L").";
        return result;
    }

    std::wstring canonicalPath(requiredLength, L'\0');
    DWORD writtenLength =
        GetFinalPathNameByHandleW(file.get(), canonicalPath.data(), requiredLength, FILE_NAME_NORMALIZED);
    if (writtenLength == 0 || writtenLength >= requiredLength) {
        result.error = L"The file's canonical path could not be determined (Windows error "
            + std::to_wstring(GetLastError()) + L").";
        return result;
    }
    canonicalPath.resize(writtenLength);

    result.file = std::move(file);
    result.canonicalPath = std::move(canonicalPath);
    return result;
}

std::optional<platform::Win32Handle> DuplicateInheritableHandle(HANDLE source)
{
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate, 0,
                          /*bInheritHandle=*/TRUE, DUPLICATE_SAME_ACCESS)) {
        return std::nullopt;
    }
    return platform::Win32Handle(duplicate);
}

std::optional<uint64_t> DuplicateHandleIntoProcess(HANDLE source, HANDLE targetProcess)
{
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), source, targetProcess, &duplicate, 0,
                          /*bInheritHandle=*/FALSE, DUPLICATE_SAME_ACCESS)) {
        return std::nullopt;
    }
    // `duplicate`'s value is meaningful only in targetProcess's own handle
    // table (DuplicateHandle's documented cross-process behavior) -- same
    // convention as WorkerPool::DuplicateSectionIntoWorker.
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(duplicate));
}

} // namespace import_broker

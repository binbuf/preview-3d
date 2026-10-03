#include "import_broker/SourceFileAccess.h"

#include "platform/SourcePathPolicy.h"

#include <utility>

namespace import_broker {

OpenSourceFileResult OpenAndCanonicalizeSourceFile(const std::wstring& path)
{
    OpenSourceFileResult result;

    // Classify before any CreateFileW. A forward-slash UNC ("//server/share"),
    // an extended UNC ("\\?\UNC\server\..."), a device namespace ("\\.\...")
    // and a relative/drive-relative path are rejected here, so CreateFileW can
    // never initiate an SMB/device open that is only caught by the post-open
    // canonical check. The classifier normalizes separators on a copy; the
    // original text is opened unchanged.
    switch (platform::ClassifySourcePath(path)) {
    case platform::SourcePathKind::RemoteOrDevice:
        result.errorCode = model_core::ImportErrorCode::UnsafeReference;
        result.error = L"Choose a model and its sidecars stored on a local drive.";
        return result;
    case platform::SourcePathKind::Relative:
        result.errorCode = model_core::ImportErrorCode::UnsafeReference;
        result.error = L"Choose a model using an absolute path on a local drive.";
        return result;
    case platform::SourcePathKind::LocalAbsolute:
        break;
    }

    const bool extended = path.rfind(L"\\\\?\\",0) == 0;
    const size_t driveOffset = extended ? 4 : 0;

    // A ':' that is not the volume separator is an NTFS alternate data
    // stream (`file.txt:stream`), a drive-relative path (`C:foo`), or a URI
    // scheme. CreateFileW would silently resolve an ADS suffix to a hidden
    // stream of the same file, so reject it here exactly as
    // ResolveSidecarPath already rejects one in a sidecar reference. (A
    // drive-relative path is already rejected by the classifier above; the
    // rest of this check is for ADS and extra colons.)
    const size_t firstColon = path.find(L':');
    if (firstColon != std::wstring::npos) {
        const size_t volumeColon = extended ? 5 : 1;
        const bool extraColon = path.find(L':', firstColon + 1) != std::wstring::npos;
        const bool driveRelative = firstColon + 1 >= path.size()
            || (path[firstColon + 1] != L'\\' && path[firstColon + 1] != L'/');
        if (firstColon != volumeColon || extraColon || driveRelative) {
            result.errorCode = model_core::ImportErrorCode::UnsafeReference;
            result.error = L"Alternate data streams are not supported. Save a local copy and retry.";
            return result;
        }
    }

    // The classifier admits any drive letter; a mounted network share is still
    // a remote open, so probe the volume before CreateFileW reaches it.
    if (path.size() >= driveOffset+3 && path[driveOffset+1] == L':'
        && GetDriveTypeW((path.substr(driveOffset,2)+L"\\").c_str()) == DRIVE_REMOTE) {
        result.errorCode = model_core::ImportErrorCode::UnsafeReference;
        result.error = L"Remote and device paths are not supported. Save a local copy and retry.";
        return result;
    }

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
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileType(file.get()) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.get(), &information)
        || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        result.error = L"Choose a local regular model file.";
        return result;
    }
    if (canonicalPath.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        result.errorCode = model_core::ImportErrorCode::UnsafeReference;
        result.error = L"Remote files are not supported. Save a local copy and retry.";
        return result;
    }
    if (!information.nFileSizeHigh && !information.nFileSizeLow) {
        result.errorCode = model_core::ImportErrorCode::EmptyGeometry;
        result.error = L"The file is empty. Export a model with triangles or points and retry.";
        return result;
    }

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

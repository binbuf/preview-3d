#include "import_broker/SidecarPathResolver.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>

namespace import_broker {

namespace {

using model_core::ImportErrorCode;

SidecarResolution Reject(ImportErrorCode code, std::wstring message)
{
    SidecarResolution result;
    result.rejectionCode = code;
    result.diagnosticMessage = std::move(message);
    return result;
}

std::wstring Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    int required = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), required);
    return wide;
}

std::wstring LowerCopy(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

bool HasAllowedSidecarExtension(const std::filesystem::path& path)
{
    static const std::wstring kAllowed[] = { L".bin", L".png", L".jpg", L".jpeg", L".webp", L".ktx2" };
    std::wstring ext = LowerCopy(path.extension().wstring());
    for (const auto& allowed : kAllowed) {
        if (ext == allowed) {
            return true;
        }
    }
    return false;
}

} // namespace

SidecarResolution ResolveSidecarPath(const std::wstring& primaryCanonicalPath,
                                      const std::string& relativeReferenceUtf8,
                                      uint64_t maxSidecarFileBytes)
{
    if (relativeReferenceUtf8.empty()) {
        return Reject(ImportErrorCode::UnsafeReference, L"empty sidecar reference");
    }

    // One conservative rule covering three distinct Input-boundary concerns
    // at once: a drive-relative path ("C:foo"), an alternate data stream
    // ("file:stream"), and any URI scheme ("data:", "http:", "file:", ...).
    // Stricter than the doc's letter, strictly safer than handling each
    // separately.
    if (relativeReferenceUtf8.find(':') != std::string::npos) {
        return Reject(ImportErrorCode::UnsafeReference, L"sidecar reference contains ':'");
    }

    std::wstring reference = Utf8ToWide(relativeReferenceUtf8);
    if (reference.empty()) {
        return Reject(ImportErrorCode::UnsafeReference, L"sidecar reference failed to decode as UTF-8");
    }

    // UNC/device-path prefix ("\\server\share", "\\.\", "\\?\") -- none of
    // these contain ':' immediately, so this is a separate check.
    if (reference.size() >= 2 && (reference[0] == L'\\' || reference[0] == L'/')
        && (reference[1] == L'\\' || reference[1] == L'/')) {
        return Reject(ImportErrorCode::UnsafeReference, L"sidecar reference is a UNC/device path");
    }

    std::filesystem::path referencePath(reference);
    if (referencePath.is_absolute() || referencePath.has_root_name() || referencePath.has_root_directory()) {
        return Reject(ImportErrorCode::UnsafeReference, L"sidecar reference is absolute/rooted");
    }
    for (const auto& component : referencePath) {
        if (component == L"..") {
            return Reject(ImportErrorCode::UnsafeReference, L"sidecar reference contains a '..' component");
        }
    }
    if (!HasAllowedSidecarExtension(referencePath)) {
        return Reject(ImportErrorCode::UnsafeReference, L"sidecar reference has a disallowed extension");
    }

    std::filesystem::path primaryDirectory = std::filesystem::path(primaryCanonicalPath).parent_path();
    std::filesystem::path joined = primaryDirectory / referencePath;

    HANDLE rawFile = CreateFileW(joined.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (rawFile == INVALID_HANDLE_VALUE) {
        return Reject(ImportErrorCode::FileUnavailable,
                       L"sidecar file could not be opened (Windows error " + std::to_wstring(GetLastError())
                           + L")");
    }
    platform::Win32Handle file(rawFile);

    DWORD requiredLength = GetFinalPathNameByHandleW(file.get(), nullptr, 0, FILE_NAME_NORMALIZED);
    if (requiredLength == 0) {
        return Reject(ImportErrorCode::FileUnavailable, L"sidecar canonical path could not be determined");
    }
    std::wstring canonicalPath(requiredLength, L'\0');
    DWORD writtenLength = GetFinalPathNameByHandleW(file.get(), canonicalPath.data(), requiredLength,
                                                      FILE_NAME_NORMALIZED);
    if (writtenLength == 0 || writtenLength >= requiredLength) {
        return Reject(ImportErrorCode::FileUnavailable, L"sidecar canonical path could not be determined");
    }
    canonicalPath.resize(writtenLength);

    // Reparse points resolved after opening -- compare the resulting
    // canonical *directory* against the primary's own, full string
    // compare (not a prefix check a sibling like "C:\Foo2" could satisfy
    // against "C:\Foo").
    std::filesystem::path primaryCanonicalDirectory
        = std::filesystem::path(primaryCanonicalPath).parent_path();
    std::filesystem::path sidecarCanonicalDirectory = std::filesystem::path(canonicalPath).parent_path();
    if (LowerCopy(sidecarCanonicalDirectory.wstring()) != LowerCopy(primaryCanonicalDirectory.wstring())) {
        return Reject(ImportErrorCode::UnsafeReference,
                       L"sidecar reference resolved outside the primary file's directory");
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0) {
        return Reject(ImportErrorCode::FileUnavailable, L"sidecar file is empty or its size could not be read");
    }
    if (static_cast<uint64_t>(size.QuadPart) > maxSidecarFileBytes) {
        return Reject(ImportErrorCode::FileUnavailable, L"sidecar file exceeds the size cap");
    }

    SidecarResolution result;
    result.file = std::move(file);
    result.canonicalPath = std::move(canonicalPath);
    result.fileSizeBytes = static_cast<uint64_t>(size.QuadPart);
    return result;
}

} // namespace import_broker

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
    static const std::wstring kAllowed[] = { L".bin", L".mtl", L".png", L".jpg", L".jpeg",
                                             L".bmp", L".tif", L".tiff", L".webp", L".ktx2",
                                             L".usd", L".usda", L".usdc" };
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
    // The primary canonical path carries the "\\?\" extended-length prefix
    // (GetFinalPathNameByHandleW's normalized form), and that prefix disables
    // Windows path normalization: a forward slash in a glTF URI
    // ("textures/foo.jpg") is not accepted by CreateFileW once joined under
    // it. The reference is already validated as relative with no "..", so
    // converting its separators to the platform's preferred form is safe.
    referencePath.make_preferred();
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

    // Containment to the primary file's directory *tree*, not just to its
    // immediate directory: a .gltf commonly references "textures/foo.jpg" or
    // "buffer/mesh.bin", so a subdirectory below the primary directory must
    // resolve. What must never resolve is anything above or beside it. The
    // reference text has already been checked for ".." and rooting, and both
    // sides here are GetFinalPathNameByHandleW-canonicalized
    // (FILE_NAME_NORMALIZED), so a reparse point that resolves outside the
    // primary directory surfaces as a canonical path outside it. Compare the
    // sidecar's full canonical path against the primary directory with a
    // trailing separator -- a plain string prefix would let a sibling like
    // "C:\Foo2" satisfy "C:\Foo", and the separator also stops a same-named
    // file from being matched component-by-component.
    std::wstring primaryDirectoryPrefix = LowerCopy(primaryDirectory.wstring());
    if (primaryDirectoryPrefix.empty() || primaryDirectoryPrefix.back() != L'\\') {
        primaryDirectoryPrefix.push_back(L'\\');
    }
    std::wstring sidecarCanonicalPath = LowerCopy(canonicalPath);
    if (sidecarCanonicalPath.size() <= primaryDirectoryPrefix.size()
        || sidecarCanonicalPath.compare(0, primaryDirectoryPrefix.size(), primaryDirectoryPrefix) != 0) {
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

#include "SafeFileOps.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <vector>

namespace preview3d::safeio
{
namespace
{
bool IsControlOrFormat(wchar_t character)
{
    const unsigned int value = static_cast<unsigned int>(character);
    if (value <= 0x1F || value == 0x7F) return true;      // C0 + DEL
    if (value >= 0x80 && value <= 0x9F) return true;      // C1
    if (value == 0xFEFF) return true;                     // BOM / zero-width no-break
    if (value >= 0x200B && value <= 0x200F) return true;  // zero-width + marks
    if (value >= 0x202A && value <= 0x202E) return true;  // bidi embeddings/overrides
    if (value >= 0x2060 && value <= 0x206F) return true;  // invisible operators + isolates
    return false;
}

bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        if (std::towlower(left[i]) != std::towlower(right[i])) return false;
    }
    return true;
}

bool StartsWithIgnoreCase(std::wstring_view text, std::wstring_view prefix)
{
    return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

bool HasDriveQualifier(std::wstring_view path)
{
    return path.size() >= 3 && std::iswalpha(path[0]) && path[1] == L':' &&
        (path[2] == L'\\' || path[2] == L'/');
}

bool HasTraversalComponent(std::wstring_view path)
{
    std::size_t start = 0;
    while (start <= path.size())
    {
        std::size_t end = path.find_first_of(L"\\/", start);
        if (end == std::wstring_view::npos) end = path.size();
        const std::wstring_view component = path.substr(start, end - start);
        if (component == L"..") return true;
        if (end == path.size()) break;
        start = end + 1;
    }
    return false;
}
} // namespace

std::wstring SanitizeDisplayText(std::wstring_view text, std::size_t maxLength)
{
    if (maxLength == 0) return {};

    bool truncated = false;
    std::wstring sanitized;
    sanitized.reserve(std::min(text.size(), maxLength));
    for (const wchar_t character : text)
    {
        if (sanitized.size() >= maxLength)
        {
            truncated = true;
            break;
        }
        sanitized.push_back(IsControlOrFormat(character) ? L' ' : character);
    }

    const auto notSpace = [](wchar_t c) { return c != L' ' && c != L'\t'; };
    const auto begin = std::find_if(sanitized.begin(), sanitized.end(), notSpace);
    const auto end = std::find_if(sanitized.rbegin(), sanitized.rend(), notSpace).base();
    if (begin >= end) return {};
    std::wstring result(begin, end);

    if (truncated)
    {
        if (result.size() >= maxLength) result.resize(maxLength - 1);
        result.push_back(L'\u2026');
    }
    return result;
}

bool AppDataDirectory(std::wstring& outRoot)
{
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) || !localAppData)
    {
        if (localAppData) CoTaskMemFree(localAppData);
        return false;
    }
    outRoot = localAppData;
    CoTaskMemFree(localAppData);
    outRoot += L"\\Binbuf\\Preview 3D";
    return true;
}

bool IsSafeOutputPath(std::wstring_view path, std::wstring_view requiredExtension,
                      std::wstring_view appOwnedRoot, std::wstring& error)
{
    const auto fail = [&error](const wchar_t* message) {
        error = message;
        return false;
    };

    if (path.empty()) return fail(L"The output path is empty.");
    if (path.size() > 4096) return fail(L"The output path is too long.");
    for (const wchar_t character : path)
    {
        if (IsControlOrFormat(character)) return fail(L"The output path contains control characters.");
    }
    if (path[0] == L'\\' || path[0] == L'/' || StartsWithIgnoreCase(path, L"\\\\.\\") ||
        StartsWithIgnoreCase(path, L"\\\\?\\"))
    {
        return fail(L"UNC and device paths are not allowed.");
    }
    if (!HasDriveQualifier(path)) return fail(L"The output path must be an absolute local path.");
    for (std::size_t i = 2; i < path.size(); ++i)
    {
        if (path[i] == L':') return fail(L"Alternate data streams and extra colons are not allowed.");
    }
    if (HasTraversalComponent(path)) return fail(L"Parent-directory traversal is not allowed.");
    if (!requiredExtension.empty())
    {
        if (path.size() < requiredExtension.size() ||
            !EqualsIgnoreCase(path.substr(path.size() - requiredExtension.size()), requiredExtension))
        {
            return fail(L"The output path has an unexpected extension.");
        }
    }
    if (!appOwnedRoot.empty())
    {
        std::wstring root(appOwnedRoot);
        while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        if (!StartsWithIgnoreCase(path, root) ||
            (path.size() > root.size() && path[root.size()] != L'\\' && path[root.size()] != L'/'))
        {
            return fail(L"The output path is outside the application directory.");
        }
    }
    return true;
}

bool WriteFileAtomically(std::wstring_view finalPath, std::span<const std::byte> bytes,
                         bool replaceExisting, std::wstring& error)
{
    error.clear();
    if (finalPath.empty())
    {
        error = L"The destination path is empty.";
        return false;
    }

    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
    {
        error = L"A unique temporary name could not be generated.";
        return false;
    }
    wchar_t guidText[40]{};
    if (StringFromGUID2(guid, guidText, static_cast<int>(std::size(guidText))) == 0)
    {
        error = L"A unique temporary name could not be generated.";
        return false;
    }

    std::wstring tempPath(finalPath);
    tempPath += L".";
    tempPath += guidText;
    tempPath += L".tmp";

    HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = L"The temporary file could not be created (Windows error " +
            std::to_wstring(GetLastError()) + L").";
        return false;
    }

    bool ok = true;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        ok = false;
        error = L"The temporary file is a reparse point.";
    }
    if (ok && !bytes.empty())
    {
        DWORD written = 0;
        ok = bytes.size() <= MAXDWORD &&
            WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size();
    }
    if (ok) ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!ok)
    {
        DeleteFileW(tempPath.c_str());
        if (error.empty()) error = L"Writing the temporary file failed.";
        return false;
    }

    const DWORD moveFlags = MOVEFILE_WRITE_THROUGH | (replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(tempPath.c_str(), std::wstring(finalPath).c_str(), moveFlags))
    {
        const DWORD lastError = GetLastError();
        DeleteFileW(tempPath.c_str());
        error = L"The output file could not be committed (Windows error " +
            std::to_wstring(lastError) + L").";
        return false;
    }
    return true;
}
} // namespace preview3d::safeio
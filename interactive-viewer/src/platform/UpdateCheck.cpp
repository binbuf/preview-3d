#include "UpdateCheck.h"
#include "VersionCompare.h"
#include "platform/Sha256.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <functional>
#include <vector>

namespace preview3d_update
{
namespace
{
std::wstring Trim(std::wstring value)
{
    const auto first = std::find_if_not(value.begin(), value.end(),
        [](wchar_t character) { return std::iswspace(character) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
        [](wchar_t character) { return std::iswspace(character) != 0; }).base();
    if (first >= last) return {};
    return std::wstring(first, last);
}

void SetError(std::wstring& error, const wchar_t* step, DWORD code)
{
    error = std::wstring(step) + L" (error " + std::to_wstring(code) + L")";
}

bool Utf8ToWide(const std::string& bytes, std::wstring& text)
{
    if (bytes.empty()) { text.clear(); return true; }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0) return false;
    text.resize(static_cast<std::size_t>(length));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
        static_cast<int>(bytes.size()), text.data(), length) == length;
}

bool isHexDigit(wchar_t character)
{
    return (character >= L'0' && character <= L'9') ||
        (character >= L'a' && character <= L'f') ||
        (character >= L'A' && character <= L'F');
}

// One bounded HTTPS GET. The response is never buffered wholesale by this
// layer: the caller decides whether to append text or stream to a file.
bool HttpGet(const std::wstring& url, std::size_t maximumBytes,
    const std::function<bool(const char*, std::size_t)>& onChunk,
    std::wstring& error, std::stop_token stop)
{
    error.clear();
    if (url.empty() || maximumBytes == 0)
    {
        error = L"invalid request";
        return false;
    }

    // WinHttpCrackUrl only fills a component's pointer when its length member
    // is non-zero; with a zeroed URL_COMPONENTS it succeeds but leaves every
    // pointer null. Set the lengths we consume to "unknown" (-1) so the
    // function points them into `url`.
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components) ||
        components.lpszHostName == nullptr)
    {
        error = L"invalid URL";
        return false;
    }

    std::wstring path;
    if (components.lpszUrlPath != nullptr && components.dwUrlPathLength > 0)
    {
        path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.lpszExtraInfo != nullptr && components.dwExtraInfoLength > 0)
    {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && components.nScheme != INTERNET_SCHEME_HTTP)
    {
        error = L"unsupported URL scheme";
        return false;
    }

    HINTERNET session = WinHttpOpen(L"Preview3D-Updater/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
    {
        SetError(error, L"WinHttpOpen failed", GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session, 3000, 3000, 3000, 5000);

    HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (connection == nullptr)
    {
        SetError(error, L"WinHttpConnect failed", GetLastError());
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, nullptr, secure ? WINHTTP_FLAG_SECURE : 0);
    if (request == nullptr)
    {
        SetError(error, L"WinHttpOpenRequest failed", GetLastError());
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    // Follow ordinary redirects (github.com -> the release asset host) but
    // never downgrade an HTTPS request to cleartext HTTP.
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    const wchar_t headers[] =
        L"Accept: text/plain, application/json, application/octet-stream\r\n"
        L"User-Agent: Preview3D-Updater/1.0\r\n";
    bool ok = WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L),
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE;
    if (!ok) SetError(error, L"WinHttpSendRequest failed", GetLastError());

    if (ok)
    {
        ok = WinHttpReceiveResponse(request, nullptr) != FALSE;
        if (!ok) SetError(error, L"WinHttpReceiveResponse failed", GetLastError());
    }

    if (ok)
    {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        ok = WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) != FALSE;
        if (!ok) SetError(error, L"WinHttpQueryHeaders failed", GetLastError());
        else if (status != 200)
        {
            error = L"HTTP status " + std::to_wstring(status);
            ok = false;
        }
    }

    std::size_t totalBytes = 0;
    while (ok)
    {
        if (stop.stop_requested())
        {
            error = L"cancelled";
            ok = false;
            break;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            SetError(error, L"WinHttpQueryDataAvailable failed", GetLastError());
            ok = false;
            break;
        }
        if (available == 0) break;
        if (available > maximumBytes - totalBytes)
        {
            error = L"response exceeded the size limit";
            ok = false;
            break;
        }
        // Bound each allocation even if a proxy reports an enormous
        // available-bytes value; the read loop simply repeats.
        const DWORD chunk = std::min<DWORD>(available, 64 * 1024);
        std::vector<char> buffer(chunk);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), chunk, &read))
        {
            SetError(error, L"WinHttpReadData failed", GetLastError());
            ok = false;
            break;
        }
        if (read == 0) break;
        totalBytes += read;
        if (!onChunk(buffer.data(), read))
        {
            ok = false;
            break;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}
}

bool DownloadText(const std::wstring& url, std::wstring& text, std::wstring& error,
    std::size_t maximumBytes)
{
    text.clear();
    std::string bytes;
    const bool ok = HttpGet(url, maximumBytes, [&bytes](const char* data, std::size_t size) {
        bytes.append(data, size);
        return true;
    }, error, {});
    if (!ok) return false;
    if (!Utf8ToWide(bytes, text))
    {
        error = L"response was not valid UTF-8";
        return false;
    }
    text = Trim(std::move(text));
    return true;
}

bool DownloadFile(const std::wstring& url, const std::wstring& path,
    std::wstring& error, std::size_t maximumBytes, std::stop_token stop)
{
    error.clear();
    if (path.empty())
    {
        error = L"invalid destination";
        return false;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        SetError(error, L"could not create the download file", GetLastError());
        return false;
    }

    DWORD writeError = 0;
    const bool ok = HttpGet(url, maximumBytes, [file, &writeError](const char* data, std::size_t size) {
        DWORD written = 0;
        if (!WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) || written != size)
        {
            writeError = GetLastError();
            return false;
        }
        return true;
    }, error, stop);

    bool flushed = ok && FlushFileBuffers(file) != FALSE;
    const DWORD flushError = flushed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok || !flushed)
    {
        if (writeError != ERROR_SUCCESS)
            SetError(error, L"could not write the download file", writeError);
        else if (ok && !flushed)
            SetError(error, L"could not flush the download file", flushError);
        else if (error.empty())
            error = L"download failed";
        DeleteFileW(path.c_str());
        return false;
    }
    return true;
}

bool ParseSha256Sidecar(const std::wstring& text, const std::wstring& expectedFileName,
    std::wstring& hash, std::wstring& error)
{
    hash.clear();
    error.clear();

    std::wstring firstLine = Trim(text);
    const std::size_t newline = firstLine.find_first_of(L"\r\n");
    if (newline != std::wstring::npos) firstLine = Trim(firstLine.substr(0, newline));
    if (firstLine.size() < 64 || !std::all_of(firstLine.begin(), firstLine.begin() + 64, isHexDigit))
    {
        error = L"checksum file did not contain a SHA-256 digest";
        return false;
    }

    if (firstLine.size() > 64 && !std::iswspace(firstLine[64]))
    {
        error = L"checksum file had an unexpected digest length";
        return false;
    }

    std::wstring fileName = Trim(firstLine.substr(64));
    if (!fileName.empty() && fileName.front() == L'*') fileName = Trim(fileName.substr(1));
    if (!expectedFileName.empty() && !fileName.empty())
    {
        // Compare only the file name portion; the published sidecar may carry
        // just the leaf name, which is what the release workflow writes.
        const std::size_t slash = fileName.find_last_of(L"\\/");
        if (slash != std::wstring::npos) fileName.erase(0, slash + 1);
        if (fileName.size() != expectedFileName.size() ||
            !std::equal(fileName.begin(), fileName.end(), expectedFileName.begin(),
                [](wchar_t left, wchar_t right) { return std::towlower(left) == std::towlower(right); }))
        {
            error = L"checksum file named a different artifact";
            return false;
        }
    }

    hash = firstLine.substr(0, 64);
    std::transform(hash.begin(), hash.end(), hash.begin(),
        [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return true;
}

bool VerifyFileSha256(const std::wstring& path, const std::wstring& expectedHex,
    std::wstring& error)
{
    error.clear();
    if (expectedHex.size() != 64 ||
        !std::all_of(expectedHex.begin(), expectedHex.end(), isHexDigit))
    {
        error = L"invalid expected SHA-256 digest";
        return false;
    }

    const auto digest = platform::ComputeFileSha256(path);
    if (!digest)
    {
        error = L"could not hash the downloaded file";
        return false;
    }

    constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring actual;
    actual.reserve(64);
    for (const std::byte byte : *digest)
    {
        const unsigned int value = static_cast<unsigned int>(byte);
        actual.push_back(kHex[(value >> 4) & 0xF]);
        actual.push_back(kHex[value & 0xF]);
    }

    const bool matches = std::equal(actual.begin(), actual.end(), expectedHex.begin(),
        [](wchar_t left, wchar_t right) { return std::towlower(left) == std::towlower(right); });
    if (!matches)
    {
        error = L"downloaded file did not match its published SHA-256 digest";
        return false;
    }
    return true;
}

std::wstring InstallerAssetName(const std::wstring& version)
{
    const std::wstring normalized = NormalizeVersion(version);
    if (normalized.empty()) return {};
    return L"Preview3D-" + normalized + L"-x64-setup.exe";
}

bool DownloadInstaller(const std::wstring& version, const std::wstring& directory,
    std::wstring& installerPath, std::wstring& expectedSha256, std::wstring& error,
    std::stop_token stop)
{
    installerPath.clear();
    expectedSha256.clear();
    error.clear();

    const std::wstring normalizedVersion = NormalizeVersion(version);
    const std::wstring assetName = InstallerAssetName(version);
    if (assetName.empty() || normalizedVersion.empty())
    {
        error = L"invalid update version";
        return false;
    }
    if (directory.empty())
    {
        error = L"invalid download directory";
        return false;
    }
    const int directoryResult = SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    if (directoryResult != ERROR_SUCCESS && directoryResult != ERROR_ALREADY_EXISTS)
    {
        error = L"could not create the update download directory (error " +
            std::to_wstring(directoryResult) + L")";
        return false;
    }

    // The concrete tag, not releases/latest, so a release published between
    // the version check and this download cannot make the asset name stale.
    const std::wstring baseUrl = std::wstring(kReleaseDownloadUrl) +
        L"v" + normalizedVersion + L"/" + assetName;
    std::wstring expectedHash;
    std::wstring sidecar;
    if (stop.stop_requested()) { error = L"cancelled"; return false; }
    if (!DownloadText(baseUrl + L".sha256", sidecar, error, 4096)) return false;
    if (!ParseSha256Sidecar(sidecar, assetName, expectedHash, error)) return false;

    const std::wstring finalPath = directory + L"\\" + assetName;
    const std::wstring partialPath = finalPath + L".part";
    DeleteFileW(partialPath.c_str());
    if (!DownloadFile(baseUrl, partialPath, error, 1024ull * 1024 * 1024, stop)) return false;
    if (stop.stop_requested())
    {
        DeleteFileW(partialPath.c_str());
        error = L"cancelled";
        return false;
    }
    if (!VerifyFileSha256(partialPath, expectedHash, error))
    {
        DeleteFileW(partialPath.c_str());
        return false;
    }
    if (!MoveFileExW(partialPath.c_str(), finalPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        SetError(error, L"could not finalize the downloaded installer", GetLastError());
        DeleteFileW(partialPath.c_str());
        return false;
    }

    installerPath = finalPath;
    expectedSha256 = std::move(expectedHash);
    return true;
}

bool FetchLatestVersion(std::wstring& version, std::wstring& error)
{
    version.clear();
    error.clear();

    // Preferred source: a tiny release asset that contains exactly the
    // version string. It needs no JSON parsing and is not rate limited like
    // the GitHub API.
    std::wstring text;
    std::wstring firstError;
    if (DownloadText(kLatestVersionUrl, text, firstError, 4096))
    {
        const std::wstring normalized = NormalizeVersion(text);
        if (!normalized.empty())
        {
            version = normalized;
            return true;
        }
        firstError = L"version.txt did not contain a valid version";
    }

    // Fallback for releases published before the version.txt asset existed.
    std::wstring json;
    std::wstring apiError;
    if (DownloadText(kLatestReleaseApiUrl, json, apiError))
    {
        const std::wstring normalized = NormalizeVersion(ExtractGitHubReleaseTag(json));
        if (!normalized.empty())
        {
            version = normalized;
            return true;
        }
        apiError = L"releases/latest did not contain a valid tag_name";
    }

    error = L"version.txt: " + firstError + L"; releases API: " + apiError;
    return false;
}
}

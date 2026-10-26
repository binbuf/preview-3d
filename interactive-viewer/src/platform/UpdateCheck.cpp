#include "UpdateCheck.h"
#include "VersionCompare.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cwctype>
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
}

bool DownloadText(const std::wstring& url, std::wstring& text, std::wstring& error,
    std::size_t maximumBytes)
{
    text.clear();
    error.clear();
    if (url.empty() || maximumBytes == 0)
    {
        error = L"invalid request";
        return false;
    }

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
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
        L"Accept: text/plain, application/json\r\n"
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

    std::string bytes;
    while (ok)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            SetError(error, L"WinHttpQueryDataAvailable failed", GetLastError());
            ok = false;
            break;
        }
        if (available == 0) break;
        if (bytes.size() + available > maximumBytes)
        {
            error = L"response exceeded the size limit";
            ok = false;
            break;
        }
        std::vector<char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read))
        {
            SetError(error, L"WinHttpReadData failed", GetLastError());
            ok = false;
            break;
        }
        bytes.append(buffer.data(), read);
        if (read == 0) break;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (!ok) return false;
    if (!Utf8ToWide(bytes, text))
    {
        error = L"response was not valid UTF-8";
        return false;
    }
    text = Trim(std::move(text));
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

#pragma once

#include <string>

// Background-free update lookup: the caller owns the worker thread and only
// receives a validated version string. All network failures are reported
// through `error` and are never user-visible: an update check must never
// interfere with opening a model.
namespace preview3d_update
{
// Stable URLs owned by the project. The release workflow publishes version.txt
// as an asset on every non-prerelease GitHub release, so
// releases/latest/download/version.txt always names the newest release. The
// API is a fallback for releases that predate that asset.
inline constexpr wchar_t kLatestVersionUrl[] =
    L"https://github.com/binbuf/preview-3d/releases/latest/download/version.txt";
inline constexpr wchar_t kLatestReleaseApiUrl[] =
    L"https://api.github.com/repos/binbuf/preview-3d/releases/latest";
inline constexpr wchar_t kReleasesPageUrl[] =
    L"https://github.com/binbuf/preview-3d/releases";

// Downloads and validates the latest published version. Returns a normalized
// MAJOR.MINOR.PATCH[-PRERELEASE] string on success. The response is bounded and
// treated as untrusted text; only a valid SemVer value is accepted.
bool FetchLatestVersion(std::wstring& version, std::wstring& error);

// Exposed for tests and diagnostics: performs one bounded HTTPS GET.
bool DownloadText(const std::wstring& url, std::wstring& text, std::wstring& error,
    std::size_t maximumBytes = 64 * 1024);
}

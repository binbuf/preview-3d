#pragma once

#include <stop_token>
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
inline constexpr wchar_t kReleaseDownloadUrl[] =
    L"https://github.com/binbuf/preview-3d/releases/download/";
inline constexpr wchar_t kReleasesPageUrl[] =
    L"https://github.com/binbuf/preview-3d/releases";

// Downloads and validates the latest published version. Returns a normalized
// MAJOR.MINOR.PATCH[-PRERELEASE] string on success. The response is bounded and
// treated as untrusted text; only a valid SemVer value is accepted.
bool FetchLatestVersion(std::wstring& version, std::wstring& error);

// Release asset naming contract shared by the release workflow and the viewer:
// Preview3D-<version>-x64-setup.exe plus its adjacent .sha256 sidecar.
std::wstring InstallerAssetName(const std::wstring& version);

// Downloads the installer for `version` into `directory`, verifies it against
// the release's published SHA-256 sidecar, and returns the verified local path
// plus the digest the handoff must re-check before elevation. The installer is
// never started here; the caller owns the handoff.
bool DownloadInstaller(const std::wstring& version, const std::wstring& directory,
    std::wstring& installerPath, std::wstring& expectedSha256, std::wstring& error,
    std::stop_token stop = {});

// Parses "hash  filename" (or "hash *filename") checksum text. Exposed for
// tests. `expectedFileName` may be empty to skip the filename check.
bool ParseSha256Sidecar(const std::wstring& text, const std::wstring& expectedFileName,
    std::wstring& hash, std::wstring& error);

// Verifies a file's SHA-256 against a 64-character hex digest.
bool VerifyFileSha256(const std::wstring& path, const std::wstring& expectedHex,
    std::wstring& error);

// Exposed for tests and diagnostics: performs one bounded HTTPS GET.
bool DownloadText(const std::wstring& url, std::wstring& text, std::wstring& error,
    std::size_t maximumBytes = 64 * 1024);

// Streams a bounded HTTPS GET straight to `path`. Used for release binaries
// that are far too large to hold in memory.
bool DownloadFile(const std::wstring& url, const std::wstring& path,
    std::wstring& error, std::size_t maximumBytes = 1024ull * 1024 * 1024,
    std::stop_token stop = {});
}

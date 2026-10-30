// Release-download contract coverage: the asset name shared with the release
// workflow, sidecar parsing, SHA-256 verification, and the command line used to
// hand control to the non-elevated updater helper.

#include "UpdateApply.h"
#include "UpdateCheck.h"

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using preview3d_update::DownloadText;
using preview3d_update::InstallerAssetName;
using preview3d_update::ParseSha256Sidecar;
using preview3d_update::QuoteWindowsCommandLineArgument;
using preview3d_update::VerifyFileSha256;

namespace {
constexpr wchar_t kEmptySha256[] =
    L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr wchar_t kAbcSha256[] =
    L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

std::wstring WriteScratchFile(const char* contents)
{
    wchar_t tempDir[MAX_PATH]{};
    REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
    wchar_t path[MAX_PATH]{};
    REQUIRE(GetTempFileNameW(tempDir, L"upd", 0, path) != 0);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    const DWORD length = static_cast<DWORD>(std::char_traits<char>::length(contents));
    DWORD written = 0;
    REQUIRE(WriteFile(file, contents, length, &written, nullptr));
    REQUIRE(written == length);
    CloseHandle(file);
    return path;
}
}

TEST_CASE("installer asset names match the release workflow", "[update]")
{
    REQUIRE(InstallerAssetName(L"1.2.3") == L"Preview3D-1.2.3-x64-setup.exe");
    REQUIRE(InstallerAssetName(L"v1.2.3") == L"Preview3D-1.2.3-x64-setup.exe");
    REQUIRE(InstallerAssetName(L"1.2.3-rc.1+build.5") == L"Preview3D-1.2.3-rc.1-x64-setup.exe");
    REQUIRE(InstallerAssetName(L"not-a-version").empty());
    REQUIRE(InstallerAssetName(L"").empty());
}

TEST_CASE("SHA-256 sidecars accept only the expected artifact", "[update]")
{
    const std::wstring name = L"Preview3D-1.2.3-x64-setup.exe";
    const std::wstring text = L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  " +
        name + L"\r\n";
    std::wstring hash;
    std::wstring error;
    REQUIRE(ParseSha256Sidecar(text, name, hash, error));
    REQUIRE(hash == kAbcSha256);
    REQUIRE(error.empty());

    REQUIRE(ParseSha256Sidecar(kEmptySha256, L"", hash, error));
    REQUIRE(hash == kEmptySha256);

    REQUIRE(ParseSha256Sidecar(L"deadbeef  " + name, name, hash, error) == false);
    REQUIRE(ParseSha256Sidecar(std::wstring(kEmptySha256) + L"  other.exe", name, hash, error) == false);
    REQUIRE(ParseSha256Sidecar(std::wstring(kEmptySha256) + L"  Preview3D-1.2.3-x64-setup.exe", name, hash, error));
}

TEST_CASE("download verification hashes the file on disk", "[update]")
{
    const std::wstring empty = WriteScratchFile("");
    const std::wstring abc = WriteScratchFile("abc");
    std::wstring error;

    REQUIRE(VerifyFileSha256(empty, kEmptySha256, error));
    REQUIRE(VerifyFileSha256(abc, kAbcSha256, error));

    REQUIRE_FALSE(VerifyFileSha256(abc, kEmptySha256, error));
    REQUIRE_FALSE(VerifyFileSha256(abc, L"not-a-digest", error));

    DeleteFileW(empty.c_str());
    DeleteFileW(abc.c_str());
}

TEST_CASE("update URLs are cracked before the scheme is rejected", "[update]")
{
    // Regression: WinHttpCrackUrl leaves every component pointer null when the
    // URL_COMPONENTS length members are zero, so a perfectly valid URL was
    // reported as "invalid URL" and the update check silently never ran. A
    // non-HTTP scheme exercises URL parsing without touching the network.
    std::wstring text;
    std::wstring error;
    REQUIRE_FALSE(DownloadText(L"ftp://example.com/version.txt", text, error, 4096));
    REQUIRE(error == L"unsupported URL scheme");
}

TEST_CASE("updater command line quoting follows Windows rules", "[update]")
{
    REQUIRE(QuoteWindowsCommandLineArgument(L"") == L"\"\"");
    REQUIRE(QuoteWindowsCommandLineArgument(L"C:\\Program Files\\Preview 3D\\Preview3D.exe") ==
        L"\"C:\\Program Files\\Preview 3D\\Preview3D.exe\"");
    REQUIRE(QuoteWindowsCommandLineArgument(L"plain") == L"plain");
    REQUIRE(QuoteWindowsCommandLineArgument(L"a\\b") == L"a\\b");
    REQUIRE(QuoteWindowsCommandLineArgument(L"a b\\") == L"\"a b\\\\\"");
    REQUIRE(QuoteWindowsCommandLineArgument(L"say \"hi\"") == L"\"say \\\"hi\\\"\"");
}

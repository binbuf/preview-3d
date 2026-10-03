// SEC-11 (T11) viewer local attack-surface regression coverage:
//  - control-character sanitization of worker-supplied asset references
//  - hostile --benchmark-result path rejection (the shared validator)
//  - reparse-point-resistant atomic writes used by Settings/Open With/DerivedCache

#include "SafeFileOps.h"

#include "platform/SourcePathPolicy.h"

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

#ifndef PREVIEW3D_TEST_ASSETS_DIR
#define PREVIEW3D_TEST_ASSETS_DIR L".\\"
#endif

namespace {

std::wstring ViewerExecutablePath()
{
    wchar_t module[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    std::wstring path(module);
    const std::size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return {};
    return path.substr(0, separator + 1) + L"Preview3D.exe";
}

std::wstring ScratchDirectory()
{
    wchar_t tempDir[MAX_PATH]{};
    REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
    wchar_t unique[MAX_PATH]{};
    REQUIRE(GetTempFileNameW(tempDir, L"sfo", 0, unique) != 0);
    DeleteFileW(unique);
    REQUIRE(CreateDirectoryW(unique, nullptr));
    return unique;
}

void RemoveFileQuiet(const std::wstring& path)
{
    DeleteFileW(path.c_str());
}

std::vector<std::byte> ReadBytes(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    LARGE_INTEGER size{};
    REQUIRE(GetFileSizeEx(file, &size));
    std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (!bytes.empty()) {
        REQUIRE(ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr));
        REQUIRE(read == bytes.size());
    }
    CloseHandle(file);
    return bytes;
}

std::span<const std::byte> AsBytes(const std::string& text)
{
    return { reinterpret_cast<const std::byte*>(text.data()), text.size() };
}

// Creates a symlink and returns whether the environment allows it. Many CI
// hosts lack Developer Mode / SeCreateSymbolicLinkPrivilege; callers SKIP.
bool TryCreateLink(const std::wstring& link, const std::wstring& target)
{
    return CreateSymbolicLinkW(link.c_str(), target.c_str(),
        SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
}

} // namespace

TEST_CASE("The viewer CLI rejects a hostile --benchmark-result target", "[security][cli]")
{
    const std::wstring exe = ViewerExecutablePath();
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SKIP("Preview3D.exe has not been built next to Tests.Unit");
    }

    const std::wstring fixture = PREVIEW3D_TEST_ASSETS_DIR L"tri_tight.glb";
    if (GetFileAttributesW(fixture.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SKIP("benchmark fixture is not present");
    }

    std::wstring command = L"\"" + exe + L"\" --benchmark=\"" + fixture +
        L"\" --benchmark-result=\\\\attacker\\share\\evil.json";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process));
    const DWORD wait = WaitForSingleObject(process.hProcess, 20000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 0xDEAD);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD exitCode = 0xFFFFFFFF;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    REQUIRE(wait == WAIT_OBJECT_0);
    CHECK(exitCode == 64);
}

TEST_CASE("SanitizeDisplayText strips control and bidi characters", "[security][sanitize]")
{
    std::wstring hostile = L"assets\r\nbad\u202E evil\u0007name";
    hostile.push_back(L'\0');
    hostile += L".png";
    const std::wstring clean = preview3d::safeio::SanitizeDisplayText(hostile, 256);
    CHECK(clean.find(L'\r') == std::wstring::npos);
    CHECK(clean.find(L'\n') == std::wstring::npos);
    CHECK(clean.find(L'\u0007') == std::wstring::npos);
    CHECK(clean.find(L'\u202E') == std::wstring::npos);
    CHECK(clean.find(L'\0') == std::wstring::npos);
    CHECK(clean.find(L"assets") != std::wstring::npos);
    CHECK(clean.find(L".png") != std::wstring::npos);
}

TEST_CASE("SanitizeDisplayText bounds length and trims", "[security][sanitize]")
{
    CHECK(preview3d::safeio::SanitizeDisplayText(L"   ", 512).empty());

    const std::wstring longText(1000, L'a');
    const std::wstring bounded = preview3d::safeio::SanitizeDisplayText(longText, 32);
    CHECK(bounded.size() <= 32);
    CHECK(bounded.back() == L'\u2026');

    const std::wstring exact = preview3d::safeio::SanitizeDisplayText(L"short", 32);
    CHECK(exact == L"short");
}

TEST_CASE("IsSafeOutputPath accepts an app-owned json target", "[security][safe-path]")
{
    std::wstring root;
    REQUIRE(preview3d::safeio::AppDataDirectory(root));
    const std::wstring good = root + L"\\benchmarks\\run-1.json";
    std::wstring error;
    CHECK(preview3d::safeio::IsSafeOutputPath(good, L".json", root, error));
    CHECK(error.empty());
}

TEST_CASE("IsSafeOutputPath rejects hostile benchmark-result targets", "[security][safe-path]")
{
    const std::wstring root = L"C:\\Users\\test\\AppData\\Local\\Binbuf\\Preview 3D";
    const std::wstring good = root + L"\\benchmarks\\run.json";
    const std::wstring cases[] = {
        L"",
        L"\\\\attacker\\share\\run.json",            // UNC
        L"\\\\?\\C:\\Windows\\Temp\\run.json",        // device path
        L"C:\\Windows\\Temp\\run.json",               // outside app root
        good + L":stream",                            // ADS
        root + L"\\..\\..\\Windows\\run.json",        // traversal
        root + L"\\benchmarks\\run.txt",              // wrong extension
        root + L"\\benchmarks\\run.json\u0007",       // control char
        L"relative\\run.json",                         // not absolute
    };
    std::size_t index = 0;
    for (const std::wstring& candidate : cases) {
        std::wstring error;
        INFO("hostile case " << index);
        CHECK_FALSE(preview3d::safeio::IsSafeOutputPath(candidate, L".json", root, error));
        CHECK_FALSE(error.empty());
        ++index;
    }
}

TEST_CASE("ClassifySourcePath rejects remote/device paths before BeginOpen opens them",
    "[security][source-path]")
{
    // T26: the viewer guard must refuse these before the import worker is
    // asked to open anything. Forward-slash and extended UNC forms are the
    // cases the old "\\"-prefix check missed.
    const wchar_t* remoteOrDevice[] = {
        L"//server/share/model.glb",
        L"\\\\server\\share\\model.glb",
        L"\\\\?\\UNC\\server\\share\\model.glb",
        L"\\\\.\\C:\\model.glb",
        L"\\\\.\\PhysicalDrive0",
        L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume0\\model.glb",
        L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\model.glb",
    };
    std::size_t index = 0;
    for (const wchar_t* candidate : remoteOrDevice) {
        INFO("remote/device case " << index);
        CHECK(platform::ClassifySourcePath(candidate) == platform::SourcePathKind::RemoteOrDevice);
        ++index;
    }

    const wchar_t* relative[] = {
        L"",
        L"model.glb",
        L"models\\model.glb",
        L"models/model.glb",
        L"\\model.glb",
        L"/model.glb",
        L"C:model.glb",
    };
    index = 0;
    for (const wchar_t* candidate : relative) {
        INFO("relative case " << index);
        CHECK(platform::ClassifySourcePath(candidate) == platform::SourcePathKind::Relative);
        ++index;
    }
}

TEST_CASE("ClassifySourcePath accepts local absolute drive paths", "[security][source-path]")
{
    const wchar_t* local[] = {
        L"C:\\models\\model.glb",
        L"C:/models/model.glb",
        L"c:\\models\\model.glb",
        L"\\\\?\\C:\\models\\model.glb",
        L"\\\\?\\c:/models/model.glb",
    };
    std::size_t index = 0;
    for (const wchar_t* candidate : local) {
        INFO("local case " << index);
        CHECK(platform::ClassifySourcePath(candidate) == platform::SourcePathKind::LocalAbsolute);
        ++index;
    }
}

TEST_CASE("WriteFileAtomically round-trips and replaces", "[security][atomic-write]")
{
    const std::wstring dir = ScratchDirectory();
    const std::wstring finalPath = dir + L"\\settings.json";

    std::wstring error;
    REQUIRE(preview3d::safeio::WriteFileAtomically(finalPath, AsBytes("first"), true, error));
    CHECK(std::string(reinterpret_cast<const char*>(ReadBytes(finalPath).data()), 5) == "first");

    REQUIRE(preview3d::safeio::WriteFileAtomically(finalPath, AsBytes("second"), true, error));
    CHECK(std::string(reinterpret_cast<const char*>(ReadBytes(finalPath).data()), 6) == "second");

    // No fixed-name ".tmp" residue anywhere in the scratch directory.
    WIN32_FIND_DATAW findData{};
    const std::wstring pattern = dir + L"\\*.tmp";
    HANDLE find = FindFirstFileW(pattern.c_str(), &findData);
    CHECK(find == INVALID_HANDLE_VALUE);
    if (find != INVALID_HANDLE_VALUE) FindClose(find);

    RemoveFileQuiet(finalPath);
    RemoveDirectoryW(dir.c_str());
}

TEST_CASE("WriteFileAtomically refuses an existing destination when replacement is off", "[security][atomic-write]")
{
    const std::wstring dir = ScratchDirectory();
    const std::wstring finalPath = dir + L"\\settings.json";
    std::wstring setupError;
    REQUIRE(preview3d::safeio::WriteFileAtomically(finalPath, AsBytes("one"), true, setupError));

    std::wstring error;
    CHECK_FALSE(preview3d::safeio::WriteFileAtomically(finalPath, AsBytes("two"), false, error));
    CHECK_FALSE(error.empty());
    CHECK(std::string(reinterpret_cast<const char*>(ReadBytes(finalPath).data()), 3) == "one");

    RemoveFileQuiet(finalPath);
    RemoveDirectoryW(dir.c_str());
}

TEST_CASE("WriteFileAtomically does not follow a planted .tmp symlink", "[security][atomic-write]")
{
    const std::wstring dir = ScratchDirectory();
    const std::wstring victim = dir + L"\\victim.txt";
    const std::wstring finalPath = dir + L"\\settings.json";
    const std::wstring planted = finalPath + L".tmp";

    std::wstring setupError;
    REQUIRE(preview3d::safeio::WriteFileAtomically(victim, AsBytes("VICTIM-CONTENT"), true, setupError));
    if (!TryCreateLink(planted, victim)) {
        RemoveFileQuiet(victim);
        RemoveDirectoryW(dir.c_str());
        SKIP("symlink creation is not permitted on this host");
    }

    std::wstring error;
    REQUIRE(preview3d::safeio::WriteFileAtomically(finalPath, AsBytes("SAFE"), true, error));

    const auto victimBytes = ReadBytes(victim);
    CHECK(std::string(reinterpret_cast<const char*>(victimBytes.data()), victimBytes.size()) == "VICTIM-CONTENT");
    const auto finalBytes = ReadBytes(finalPath);
    CHECK(std::string(reinterpret_cast<const char*>(finalBytes.data()), finalBytes.size()) == "SAFE");

    RemoveFileQuiet(planted);
    RemoveFileQuiet(victim);
    RemoveFileQuiet(finalPath);
    RemoveDirectoryW(dir.c_str());
}

TEST_CASE("WriteFileAtomically replaces a symlinked destination without touching its target", "[security][atomic-write]")
{
    const std::wstring dir = ScratchDirectory();
    const std::wstring victim = dir + L"\\victim.txt";
    const std::wstring finalPath = dir + L"\\settings.json";

    std::wstring setupError;
    REQUIRE(preview3d::safeio::WriteFileAtomically(victim, AsBytes("VICTIM-CONTENT"), true, setupError));
    if (!TryCreateLink(finalPath, victim)) {
        RemoveFileQuiet(victim);
        RemoveDirectoryW(dir.c_str());
        SKIP("symlink creation is not permitted on this host");
    }

    std::wstring error;
    REQUIRE(preview3d::safeio::WriteFileAtomically(finalPath, AsBytes("SAFE"), true, error));

    const auto victimBytes = ReadBytes(victim);
    CHECK(std::string(reinterpret_cast<const char*>(victimBytes.data()), victimBytes.size()) == "VICTIM-CONTENT");
    CHECK((GetFileAttributesW(finalPath.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
    const auto finalBytes = ReadBytes(finalPath);
    CHECK(std::string(reinterpret_cast<const char*>(finalBytes.data()), finalBytes.size()) == "SAFE");

    RemoveFileQuiet(victim);
    RemoveFileQuiet(finalPath);
    RemoveDirectoryW(dir.c_str());
}
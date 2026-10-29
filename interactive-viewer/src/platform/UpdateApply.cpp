#include "UpdateApply.h"
#include "SafeFileOps.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <algorithm>

namespace preview3d_update
{
namespace
{
std::wstring ModulePath()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size())
        {
            buffer.resize(length);
            return buffer;
        }
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring DirectoryOf(const std::wstring& path)
{
    const std::size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return {};
    return path.substr(0, separator);
}

std::wstring CanonicalPath(const std::wstring& path)
{
    if (path.empty()) return {};
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()),
        buffer.data(), nullptr);
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    while (buffer.size() > 3 && (buffer.back() == L'\\' || buffer.back() == L'/'))
        buffer.pop_back();
    return buffer;
}

bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right)
{
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
        right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring NormalizeExistingFile(const std::wstring& path)
{
    if (path.empty()) return {};
    const std::wstring full = CanonicalPath(path);
    if (full.empty()) return {};
    const DWORD attributes = GetFileAttributesW(full.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return {};
    return full;
}
}

std::wstring QuoteWindowsCommandLineArgument(const std::wstring& argument)
{
    if (argument.empty()) return L"\"\"";
    const bool needsQuotes = argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needsQuotes) return argument;

    std::wstring quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (character == L'"')
        {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        }
        else
        {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0;
    }
    // Backslashes before the closing quote must be doubled.
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

bool IsInstalledCopy(std::wstring& installDirectory, std::wstring& error)
{
    installDirectory.clear();
    error.clear();

    std::wstring location(32768, L'\0');
    DWORD bytes = static_cast<DWORD>(location.size() * sizeof(wchar_t));
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE,
        L"Software\\Binbuf\\Preview3D", L"InstallLocation",
        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, location.data(), &bytes);
    if (status != ERROR_SUCCESS || bytes <= sizeof(wchar_t))
    {
        error = L"the per-machine installation location is not registered";
        return false;
    }
    location.resize(bytes / sizeof(wchar_t));
    while (!location.empty() && location.back() == L'\0') location.pop_back();

    const std::wstring registered = CanonicalPath(location);
    const std::wstring modulePath = ModulePath();
    const std::wstring runningFrom = CanonicalPath(DirectoryOf(modulePath));
    if (registered.empty() || runningFrom.empty())
    {
        error = L"the installation location could not be resolved";
        return false;
    }
    if (!EqualsIgnoreCase(registered, runningFrom))
    {
        error = L"this executable is not the installed copy";
        return false;
    }

    installDirectory = runningFrom;
    return true;
}

bool StartInstallerHandoff(const std::wstring& installerPath,
    const std::wstring& expectedSha256, const std::wstring& openPath,
    std::wstring& error)
{
    error.clear();

    if (expectedSha256.size() != 64 ||
        !std::all_of(expectedSha256.begin(), expectedSha256.end(), [](wchar_t character) {
            return (character >= L'0' && character <= L'9') ||
                (character >= L'a' && character <= L'f') ||
                (character >= L'A' && character <= L'F');
        }))
    {
        error = L"the update digest is invalid";
        return false;
    }

    std::wstring appDataRoot;
    if (!preview3d::safeio::AppDataDirectory(appDataRoot))
    {
        error = L"the application data directory is unavailable";
        return false;
    }
    std::wstring pathError;
    if (!preview3d::safeio::IsSafeOutputPath(installerPath, L".exe", appDataRoot, pathError))
    {
        error = pathError;
        return false;
    }

    const std::wstring installer = NormalizeExistingFile(installerPath);
    if (installer.empty())
    {
        error = L"the downloaded installer is missing";
        return false;
    }

    const std::wstring modulePath = ModulePath();
    if (modulePath.empty())
    {
        error = L"the application path could not be resolved";
        return false;
    }
    const std::wstring sourceScript =
        DirectoryOf(modulePath) + L"\\Apply-Preview3DUpdate.ps1";
    if (NormalizeExistingFile(sourceScript).empty())
    {
        error = L"the update helper is missing";
        return false;
    }

    // Run a copy outside the install directory: setup may remove or replace
    // the installed helper while this handoff is still in progress.
    const std::wstring updatesDirectory = appDataRoot + L"\\Updates";
    const std::wstring scriptPath = updatesDirectory + L"\\Apply-Preview3DUpdate.ps1";
    const int directoryResult = SHCreateDirectoryExW(nullptr, updatesDirectory.c_str(), nullptr);
    if (directoryResult != ERROR_SUCCESS && directoryResult != ERROR_ALREADY_EXISTS)
    {
        error = L"the update helper directory could not be created (error " +
            std::to_wstring(directoryResult) + L")";
        return false;
    }
    DeleteFileW(scriptPath.c_str());
    if (!CopyFileW(sourceScript.c_str(), scriptPath.c_str(), FALSE))
    {
        error = L"the update helper could not be staged (error " +
            std::to_wstring(GetLastError()) + L")";
        return false;
    }

    wchar_t systemDirectory[MAX_PATH]{};
    const UINT systemLength = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (systemLength == 0 || systemLength >= MAX_PATH)
    {
        error = L"the Windows system directory could not be resolved";
        return false;
    }
    const std::wstring powershell = std::wstring(systemDirectory, systemLength) +
        L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    if (NormalizeExistingFile(powershell).empty())
    {
        error = L"Windows PowerShell is unavailable";
        return false;
    }

    std::wstring command = QuoteWindowsCommandLineArgument(powershell) +
        L" -NoLogo -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass" +
        L" -File " + QuoteWindowsCommandLineArgument(scriptPath) +
        L" -ParentProcessId " + std::to_wstring(GetCurrentProcessId()) +
        L" -InstallerPath " + QuoteWindowsCommandLineArgument(installer) +
        L" -ExpectedSha256 " + QuoteWindowsCommandLineArgument(expectedSha256) +
        L" -AppPath " + QuoteWindowsCommandLineArgument(modulePath);

    const std::wstring normalizedOpenPath = NormalizeExistingFile(openPath);
    if (!normalizedOpenPath.empty())
        command += L" -OpenPath " + QuoteWindowsCommandLineArgument(normalizedOpenPath);

    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
            &startup, &process))
    {
        error = L"the update helper could not be started (error " +
            std::to_wstring(GetLastError()) + L")";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
}

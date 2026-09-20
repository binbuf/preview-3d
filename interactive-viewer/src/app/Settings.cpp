#include "framework.h"
#include "Settings.h"

#include <shlobj.h>

#include <cctype>
#include <cstdlib>
#include <string>

namespace
{
constexpr wchar_t kSettingsFileName[] = L"settings.json";
constexpr size_t kMaxSettingsBytes = 4096;

bool SettingsDirectory(std::wstring& outDirectory)
{
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) || !localAppData)
    {
        if (localAppData) CoTaskMemFree(localAppData);
        return false;
    }
    outDirectory = localAppData;
    CoTaskMemFree(localAppData);
    // Sibling of the future DerivedCache\v1 folder (04-rendering-and-streaming.md).
    outDirectory += L"\\Binbuf\\Preview 3D";
    return true;
}

bool SettingsFilePath(std::wstring& outPath)
{
    std::wstring directory;
    if (!SettingsDirectory(directory)) return false;
    outPath = directory + L"\\" + kSettingsFileName;
    return true;
}

bool ReadFileFully(const std::wstring& path, std::string& outContent)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > static_cast<LONGLONG>(kMaxSettingsBytes))
    {
        CloseHandle(file);
        return false;
    }
    outContent.resize(static_cast<size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    const BOOL ok = ReadFile(file, outContent.data(), static_cast<DWORD>(outContent.size()), &bytesRead, nullptr);
    CloseHandle(file);
    return ok && bytesRead == outContent.size();
}

// Finds `"key": <value>` in `json` and returns the raw text of <value> (up to
// the next comma/brace/whitespace), or an empty string if the key is absent.
// Deliberately not a general JSON parser: this file's shape is fixed, small,
// and produced only by this app, so a bounded substring scan is enough —
// reusing Model.cpp's importer-oriented JSON parser (built to survive
// adversarial glTF input) would be the wrong tool for a tiny trusted blob.
std::string FindJsonValue(const std::string& json, const char* key)
{
    const std::string needle = std::string("\"") + key + "\"";
    const size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) return {};
    const size_t colon = json.find(':', keyPos + needle.size());
    if (colon == std::string::npos) return {};
    size_t valueStart = colon + 1;
    while (valueStart < json.size() && std::isspace(static_cast<unsigned char>(json[valueStart]))) ++valueStart;
    size_t valueEnd = valueStart;
    while (valueEnd < json.size() && json[valueEnd] != ',' && json[valueEnd] != '}' &&
        !std::isspace(static_cast<unsigned char>(json[valueEnd])))
    {
        ++valueEnd;
    }
    return json.substr(valueStart, valueEnd - valueStart);
}
}

ViewerSettings LoadSettings()
{
    ViewerSettings settings;   // defaults if anything below fails
    std::wstring path;
    if (!SettingsFilePath(path)) return settings;
    std::string content;
    if (!ReadFileFully(path, content)) return settings;

    // Each field is recovered independently and falls back to its own
    // default when absent/malformed — this is what lets a future field be
    // added to ViewerSettings without a breaking migration for old files.
    const std::string versionText = FindJsonValue(content, "version");
    if (!versionText.empty()) settings.version = std::atoi(versionText.c_str());

    const std::string nativeText = FindJsonValue(content, "showNativeOrientation");
    if (nativeText == "true") settings.showNativeOrientation = true;
    else if (nativeText == "false") settings.showNativeOrientation = false;

    const std::string groundAxisText = FindJsonValue(content, "groundAxis");
    if (groundAxisText == "\"X\"") settings.groundAxis = GroundAxis::X;
    else if (groundAxisText == "\"Y\"") settings.groundAxis = GroundAxis::Y;
    else if (groundAxisText == "\"Z\"") settings.groundAxis = GroundAxis::Z;

    const std::string groundAxisInvertedText = FindJsonValue(content, "groundAxisInverted");
    if (groundAxisInvertedText == "true") settings.groundAxisInverted = true;
    else if (groundAxisInvertedText == "false") settings.groundAxisInverted = false;

    const std::string hideCursorText = FindJsonValue(content, "hideCursorWhileDragging");
    if (hideCursorText == "true") settings.hideCursorWhileDragging = true;
    else if (hideCursorText == "false") settings.hideCursorWhileDragging = false;

    return settings;
}

void SaveSettings(const ViewerSettings& settings)
{
    std::wstring directory;
    if (!SettingsDirectory(directory)) return;
    // Best-effort: succeeds trivially if the tree already exists; any other
    // failure just fails the write below, which is swallowed the same way.
    CreateDirectoryW((directory.substr(0, directory.find_last_of(L'\\'))).c_str(), nullptr);
    CreateDirectoryW(directory.c_str(), nullptr);

    const std::wstring finalPath = directory + L"\\" + kSettingsFileName;
    const std::wstring tempPath = finalPath + L".tmp";

    const char* groundAxis = settings.groundAxis == GroundAxis::X ? "X"
        : settings.groundAxis == GroundAxis::Y ? "Y"
        : settings.groundAxis == GroundAxis::Z ? "Z" : "Automatic";
    const std::string json = "{\n  \"version\": " + std::to_string(settings.version) +
        ",\n  \"showNativeOrientation\": " + (settings.showNativeOrientation ? "true" : "false") +
        ",\n  \"groundAxis\": \"" + groundAxis + "\"" +
        ",\n  \"groundAxisInverted\": " + (settings.groundAxisInverted ? "true" : "false") +
        ",\n  \"hideCursorWhileDragging\": " + (settings.hideCursorWhileDragging ? "true" : "false") + "\n}\n";

    HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const BOOL wrote = WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!wrote || written != json.size())
    {
        DeleteFileW(tempPath.c_str());
        return;
    }
    if (!MoveFileExW(tempPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(tempPath.c_str());
    }
}

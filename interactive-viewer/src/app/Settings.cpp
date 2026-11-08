#include "framework.h"
#include "Settings.h"
#include "SafeFileOps.h"

#include <shlobj.h>

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <span>
#include <string>
#include <system_error>

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

std::string UnquoteJson(const std::string& value)
{
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        return value.substr(1, value.size() - 2);
    return value;
}

std::string ImportOptionKey(model_core::ImportOptionFormat format, const char* name)
{
    return std::string("import.") + model_core::ImportOptionFormatName(format) + "." + name;
}

// Preserve the full double value; std::to_string's six decimals would round a
// valid 1e-9 deflection or unit scale to zero and lose it on the next load.
std::string FormatImportDouble(double value)
{
    char buffer[64]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                      std::chars_format::general);
    if (result.ec != std::errc()) return "0";
    return std::string(buffer, result.ptr);
}

// Loads one persisted option through the same closed grammar the CLI and
// broker use. An absent or invalid value leaves the default in place; a valid
// value commits the whole candidate record only after it validates.
void LoadImportOption(const std::string& content, model_core::ImportOptionsTable& table,
                      model_core::ImportOptionFormat format, const char* name)
{
    const std::string key = ImportOptionKey(format, name);
    const std::string raw = FindJsonValue(content, key.c_str());
    if (raw.empty()) return;
    std::string assignment = model_core::ImportOptionFormatName(format);
    assignment += '.';
    assignment += name;
    assignment += '=';
    assignment += UnquoteJson(raw);
    auto candidate = table;
    model_core::ImportOptionError error = model_core::ImportOptionError::None;
    if (model_core::ParseImportOptionAssignment(assignment, candidate, error))
        table = candidate;
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

    const std::string stopOrbitText = FindJsonValue(content, "stopOrbitOnInteraction");
    if (stopOrbitText == "true") settings.stopOrbitOnInteraction = true;
    else if (stopOrbitText == "false") settings.stopOrbitOnInteraction = false;

    const std::string hideUntilCompleteText = FindJsonValue(content, "hideModelUntilComplete");
    if (hideUntilCompleteText == "true") settings.hideModelUntilComplete = true;
    else if (hideUntilCompleteText == "false") settings.hideModelUntilComplete = false;

    const std::string languageText = FindJsonValue(content, "language");
    if (languageText.size() >= 2 && languageText.front() == '"' && languageText.back() == '"')
    {
        const std::string code = languageText.substr(1, languageText.size() - 2);
        // The code is a plain ASCII token; widen it directly.
        settings.language.assign(code.begin(), code.end());
    }

    const std::string toneMapText = FindJsonValue(content, "toneMapMode");
    if (!toneMapText.empty())
    {
        const int value = std::atoi(toneMapText.c_str());
        if (value >= 0 && value <= 3) settings.toneMapMode = value;
    }
    const std::string backgroundText = FindJsonValue(content, "backgroundMode");
    if (!backgroundText.empty())
    {
        const int value = std::atoi(backgroundText.c_str());
        if (value >= 0 && value <= 4) settings.backgroundMode = value;
    }
    const std::string backgroundColorText = FindJsonValue(content, "backgroundColor");
    if (!backgroundColorText.empty())
    {
        char* end = nullptr;
        const unsigned long value = std::strtoul(backgroundColorText.c_str(), &end, 0);
        if (end && end != backgroundColorText.c_str() && value <= 0x00FFFFFFul)
            settings.backgroundColor = static_cast<unsigned int>(value);
    }
    const std::string highlightText = FindJsonValue(content, "groundHighlightEnabled");
    if (highlightText == "true") settings.groundHighlightEnabled = true;
    else if (highlightText == "false") settings.groundHighlightEnabled = false;
    const std::string shadowText = FindJsonValue(content, "groundShadowEnabled");
    if (shadowText == "true") settings.groundShadowEnabled = true;
    else if (shadowText == "false") settings.groundShadowEnabled = false;
    const std::string shadowOverrideText = FindJsonValue(content, "groundShadowOverridden");
    if (shadowOverrideText == "true") settings.groundShadowOverridden = true;
    else if (shadowOverrideText == "false") settings.groundShadowOverridden = false;
    const std::string aoText = FindJsonValue(content, "ambientOcclusionEnabled");
    if (aoText == "true") settings.ambientOcclusionEnabled = true;
    else if (aoText == "false") settings.ambientOcclusionEnabled = false;
    const std::string exposureText = FindJsonValue(content, "exposure");
    if (!exposureText.empty())
    {
        char* end = nullptr;
        const float value = std::strtof(exposureText.c_str(), &end);
        if (end && end != exposureText.c_str() && std::isfinite(value))
            settings.exposure = std::clamp(value, 0.05f, 8.0f);
    }

    for (uint32_t index = 0; index <= model_core::kImportOptionFormatLast; ++index) {
        const auto format = static_cast<model_core::ImportOptionFormat>(index);
        LoadImportOption(content, settings.importOptions, format, "quality");
        LoadImportOption(content, settings.importOptions, format, "read_wire");
        LoadImportOption(content, settings.importOptions, format, "linear_deflection");
        LoadImportOption(content, settings.importOptions, format, "angular_deflection");
        LoadImportOption(content, settings.importOptions, format, "relative_deflection");
        LoadImportOption(content, settings.importOptions, format, "unit_scale");
        LoadImportOption(content, settings.importOptions, format, "subdivision_level");
    }

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

    const char* groundAxis = settings.groundAxis == GroundAxis::X ? "X"
        : settings.groundAxis == GroundAxis::Y ? "Y"
        : settings.groundAxis == GroundAxis::Z ? "Z" : "Automatic";
    // Language codes are plain ASCII tokens; keep only the ASCII bytes so the
    // narrowing from wchar_t is explicit rather than a warning-as-error.
    std::string language;
    language.reserve(settings.language.size());
    for (const wchar_t character : settings.language)
    {
        if (character < 0x80) language.push_back(static_cast<char>(character));
    }
    std::string escapedLanguage;
    escapedLanguage.reserve(language.size());
    for (const char character : language)
    {
        if (character == '"' || character == '\\') escapedLanguage.push_back('\\');
        escapedLanguage.push_back(character);
    }
    // T06: persist only validated, non-default values. The same closed grammar
    // used by the CLI and broker validates each record before a byte is
    // written, so an invalid in-memory value cannot be persisted.
    std::string importFields;
    auto appendImport = [&](model_core::ImportOptionFormat format, const char* name,
                            const std::string& value) {
        importFields += ",\n  \"" + ImportOptionKey(format, name) + "\": " + value;
    };
    for (uint32_t index = 0; index <= model_core::kImportOptionFormatLast; ++index) {
        const auto format = static_cast<model_core::ImportOptionFormat>(index);
        const model_core::ImportOptions& options = settings.importOptions[format];
        model_core::ImportOptionError error = model_core::ImportOptionError::None;
        if (!model_core::ValidateImportOptions(format, options, error)) continue;
        if (options.quality != model_core::ImportQuality::Balanced) {
            appendImport(format, "quality",
                std::string("\"") + (options.quality == model_core::ImportQuality::Draft
                    ? "draft" : "full") + "\"");
        }
        if (options.wireMode != model_core::ImportWireMode::Fallback) {
            appendImport(format, "read_wire",
                std::string("\"") + (options.wireMode == model_core::ImportWireMode::On
                    ? "on" : "off") + "\"");
        }
        if (options.linearDeflection)
            appendImport(format, "linear_deflection", FormatImportDouble(*options.linearDeflection));
        if (options.angularDeflection)
            appendImport(format, "angular_deflection", FormatImportDouble(*options.angularDeflection));
        if (!options.relativeDeflection)
            appendImport(format, "relative_deflection", "false");
        if (options.unitScaleOverride)
            appendImport(format, "unit_scale", FormatImportDouble(*options.unitScaleOverride));
        if (options.usdSubdivisionLevel != 0)
            appendImport(format, "subdivision_level",
                         std::to_string(options.usdSubdivisionLevel));
    }

    const std::string json = "{\n  \"version\": " + std::to_string(settings.version) +
        ",\n  \"showNativeOrientation\": " + (settings.showNativeOrientation ? "true" : "false") +
        ",\n  \"groundAxis\": \"" + groundAxis + "\"" +
        ",\n  \"groundAxisInverted\": " + (settings.groundAxisInverted ? "true" : "false") +
        ",\n  \"hideCursorWhileDragging\": " + (settings.hideCursorWhileDragging ? "true" : "false") +
        ",\n  \"stopOrbitOnInteraction\": " + (settings.stopOrbitOnInteraction ? "true" : "false") +
        ",\n  \"hideModelUntilComplete\": " + (settings.hideModelUntilComplete ? "true" : "false") +
        ",\n  \"toneMapMode\": " + std::to_string(settings.toneMapMode) +
        ",\n  \"backgroundMode\": " + std::to_string(settings.backgroundMode) +
        ",\n  \"backgroundColor\": " + std::to_string(settings.backgroundColor & 0x00FFFFFFu) +
        ",\n  \"groundHighlightEnabled\": " + (settings.groundHighlightEnabled ? "true" : "false") +
        ",\n  \"exposure\": " + std::to_string(settings.exposure) +
        ",\n  \"groundShadowEnabled\": " + (settings.groundShadowEnabled ? "true" : "false") +
        ",\n  \"groundShadowOverridden\": " + (settings.groundShadowOverridden ? "true" : "false") +
        ",\n  \"ambientOcclusionEnabled\": " + (settings.ambientOcclusionEnabled ? "true" : "false") +
        ",\n  \"language\": \"" + escapedLanguage + "\"" + importFields + "\n}\n";

    std::wstring writeError;
    preview3d::safeio::WriteFileAtomically(finalPath, std::as_bytes(std::span(json)), true, writeError);
}

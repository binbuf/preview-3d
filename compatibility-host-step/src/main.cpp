#include "StepSpikeProtocol.h"
#include "StepXdeSpike.h"

#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace {

bool ReadExact(HANDLE handle, void* buffer, DWORD length)
{
    auto* bytes = static_cast<std::byte*>(buffer);
    DWORD total = 0;
    while (total < length) {
        DWORD read = 0;
        if (!ReadFile(handle, bytes + total, length - total, &read, nullptr) || !read) return false;
        total += read;
    }
    return true;
}

bool WriteExact(HANDLE handle, const void* buffer, DWORD length)
{
    const auto* bytes = static_cast<const std::byte*>(buffer);
    DWORD total = 0;
    while (total < length) {
        DWORD written = 0;
        if (!WriteFile(handle, bytes + total, length - total, &written, nullptr) || !written)
            return false;
        total += written;
    }
    return true;
}

std::filesystem::path ExecutableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

// Fixed, build-independent hardening: system + explicitly added directories
// only, no current-directory search, and every OCCT ambient configuration
// escape hatch cleared before any resource or plug-in is consulted.
bool HardenProcessDiscovery(const std::filesystem::path& directory)
{
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS))
        return false;
    if (!AddDllDirectory(directory.c_str())) return false;
    if (!SetCurrentDirectoryW(directory.c_str())) return false;
    constexpr const wchar_t* variables[] = {
        L"CSF_OCCTResourcePath", L"CSF_PluginPath", L"CSF_UnitsLexicon",
        L"CSF_DefaultUnit", L"CSF_UnitsDefinition", L"CSF_IGESDefaults",
        L"CSF_STEPDefaults", L"CSF_XCAFDefaults", L"CSF_DrawPluginPath",
        L"CSF_MDTVTexturesDirectory", L"CSF_ShadersDirectory",
        L"CSF_GraphicShr", L"MMGT_OPT", L"PATH"
    };
    for (const auto variable : variables) SetEnvironmentVariableW(variable, nullptr);
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    using namespace step_host;
    if (argc != 4 || std::wstring_view(argv[1]) != L"--step-001-spike") return 64;

    wchar_t* end = nullptr;
    const auto handleValue = _wcstoui64(argv[2], &end, 10);
    if (!end || *end || !handleValue) return 65;
    end = nullptr;
    const auto capacity = _wcstoui64(argv[3], &end, 10);
    if (!end || *end || capacity < sizeof(StepSpikeSection) || capacity > kStepSpikeMaxSectionBytes)
        return 66;

    const auto directory = ExecutableDirectory();
    if (directory.empty() || !HardenProcessDiscovery(directory)) return 67;

    auto view = platform::MappedView::Map(
        reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handleValue)),
        FILE_MAP_READ | FILE_MAP_WRITE, static_cast<std::size_t>(capacity));
    if (!view) return 68;

    StepSpikeControl control{};
    if (!ReadExact(GetStdHandle(STD_INPUT_HANDLE), &control, sizeof(control))
        || control.magic != kStepSpikeMagic || control.version != kStepSpikeVersion) return 69;

    auto& header = *reinterpret_cast<StepSpikeSection*>(view.bytes().data());
    const int result = RunStepSpike(header);
    WriteExact(GetStdHandle(STD_OUTPUT_HANDLE), &control, sizeof(control));
    return result;
}

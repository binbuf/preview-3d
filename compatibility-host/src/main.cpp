#include "OpenUsdHost.h"

#include "platform/MappedView.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
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
        if (!WriteFile(handle, bytes + total, length - total, &written, nullptr) || !written) return false;
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

} // namespace

int wmain(int argc, wchar_t** argv)
{
    using namespace compatibility_host;
    if (argc != 4 || std::wstring_view(argv[1]) != L"--usd-002-spike") return 64;
    const auto directory = ExecutableDirectory();
    if (directory.empty()
        || !SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS)) return 65;
    const auto dllDirectory = AddDllDirectory(directory.c_str());
    if (!dllDirectory || !SetCurrentDirectoryW(directory.c_str())) return 66;

    // OpenUSD was compiled to use only the renamed plugin-path variable. It
    // is cleared before the core loads the monolithic DLL; common
    // discovery variables are cleared as defense in depth.
    constexpr const wchar_t* variables[] = {
        L"PREVIEW3D_DISABLED_PLUGIN_PATH", L"PXR_PLUGINPATH_NAME",
        L"PXR_AR_DEFAULT_SEARCH_PATH", L"PYTHONPATH", L"USDIMAGING_ENABLE_PLUGINS",
        L"MATERIALX_SEARCH_PATH", L"RMANTREE", L"RMAN_RIXPLUGINPATH"
    };
    for (const auto variable : variables) SetEnvironmentVariableW(variable, nullptr);

    wchar_t* end = nullptr;
    const auto handleValue = _wcstoui64(argv[2], &end, 10);
    if (!end || *end || !handleValue) return 67;
    end = nullptr;
    const auto capacity = _wcstoui64(argv[3], &end, 10);
    if (!end || *end || capacity < sizeof(OpenUsdSpikeSection) || capacity > kOpenUsdMaxSectionBytes)
        return 68;
    auto view = platform::MappedView::Map(
        reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handleValue)),
        FILE_MAP_READ | FILE_MAP_WRITE, static_cast<std::size_t>(capacity));
    if (!view) return 69;

    OpenUsdSpikeControl control{};
    if (!ReadExact(GetStdHandle(STD_INPUT_HANDLE), &control, sizeof(control))
        || control.magic != kOpenUsdSpikeMagic || control.version != kOpenUsdSpikeVersion) return 70;
    const auto corePath = directory / L"Preview3DOpenUsdCore.dll";
    const HMODULE core = LoadLibraryExW(corePath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!core) return 71;
    using RunFunction = int (__cdecl*)(OpenUsdSpikeSection*, const std::byte*, std::size_t,
                                       const wchar_t*);
    const auto run = reinterpret_cast<RunFunction>(GetProcAddress(core, "Preview3DRunOpenUsdSpike"));
    if (!run) return 72;
    auto& header = *reinterpret_cast<OpenUsdSpikeSection*>(view.bytes().data());
    const int result = run(&header, view.bytes().data(), view.bytes().size(), directory.c_str());
    WriteExact(GetStdHandle(STD_OUTPUT_HANDLE), &control, sizeof(control));
    FreeLibrary(core);
    RemoveDllDirectory(dllDirectory);
    return result;
}

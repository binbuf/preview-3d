#include "OpenUsdHost.h"

#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "model_core/ImportError.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
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

bool HardenProcessDiscovery(const std::filesystem::path& directory)
{
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS))
        return false;
    if (!AddDllDirectory(directory.c_str()) || !SetCurrentDirectoryW(directory.c_str()))
        return false;

    // Lock every known OpenUSD/plugin escape hatch before a production
    // request loads Preview3DOpenUsdCore.dll. USD-006 loads it only to audit
    // the private payload; USD-007 may open a stage only after this succeeds.
    constexpr const wchar_t* variables[] = {
        L"PREVIEW3D_DISABLED_PLUGIN_PATH", L"PXR_PLUGINPATH_NAME",
        L"PXR_AR_DEFAULT_SEARCH_PATH", L"PYTHONPATH", L"USDIMAGING_ENABLE_PLUGINS",
        L"MATERIALX_SEARCH_PATH", L"RMANTREE", L"RMAN_RIXPLUGINPATH"
    };
    for (const auto variable : variables) SetEnvironmentVariableW(variable, nullptr);
    return true;
}

bool SendError(uint64_t generationId, model_core::ImportErrorCode code)
{
    model_core::GenerationErrorNotice notice{};
    notice.generationId = generationId;
    notice.errorCode = static_cast<uint32_t>(code);
    return model_core::WriteControlMessage(
        GetStdHandle(STD_OUTPUT_HANDLE), model_core::ControlOpcode::GenerationError,
        &notice, sizeof(notice));
}

HMODULE LoadAndAuditProductionPayload(const std::filesystem::path& directory)
{
    static HMODULE core = nullptr;
    static bool audited = false;
    if (audited) return core;
    if (!core) {
        const auto corePath = directory / L"Preview3DOpenUsdCore.dll";
        core = LoadLibraryExW(corePath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32
                | LOAD_LIBRARY_SEARCH_USER_DIRS);
    }
    if (!core) return nullptr;
    using AuditFunction = int (__cdecl*)(const wchar_t*);
    const auto audit = reinterpret_cast<AuditFunction>(
        GetProcAddress(core, "Preview3DAuditOpenUsdPayload"));
    if (!audit || audit(directory.c_str()) != 0) return nullptr;
    audited = true;
    return core;
}

enum class PoolMode { Normal, Crash, Hang, ConsumeMemory, StaleReply, ReverseFallback };

int RunProductionPool(PoolMode mode, const std::filesystem::path& directory)
{
    for (;;) {
        const auto received = model_core::ReadControlMessage(GetStdHandle(STD_INPUT_HANDLE));
        if (!received) return 73;
        const auto opcode = static_cast<model_core::ControlOpcode>(received->header.opcode);
        if (opcode == model_core::ControlOpcode::Shutdown && received->payload.empty())
            return 0;
        if (opcode != model_core::ControlOpcode::StartOpenUsdImportFromFile
            || received->payload.size() != sizeof(model_core::ParseOpenUsdFileRequest))
            return 74;

        model_core::ParseOpenUsdFileRequest request{};
        std::memcpy(&request, received->payload.data(), sizeof(request));
        if (mode == PoolMode::Crash) {
            RaiseFailFastException(nullptr, nullptr, 0);
            return 79;
        }
        if (mode == PoolMode::Hang) {
            Sleep(INFINITE);
            return 80;
        }
        if (mode == PoolMode::ConsumeMemory) {
            for (;;) {
                void* allocation = VirtualAlloc(nullptr, 16 * 1024 * 1024,
                                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (!allocation) break;
                std::memset(allocation, 0x5a, 16 * 1024 * 1024);
            }
            return 81;
        }
        if (mode == PoolMode::StaleReply) {
            SendError(request.generationId + 1,
                      model_core::ImportErrorCode::CompatibilityHostFailure);
            continue;
        }
        if (mode == PoolMode::ReverseFallback) {
            SendError(request.generationId,
                      model_core::ImportErrorCode::UnsupportedComposition);
            continue;
        }
        const uint32_t expected = request.requestFlags & model_core::kImportRequestUsdExpectedMask;
        const bool oneExpectedBit = expected == 0 || expected == model_core::kImportRequestUsdExpectedUsda
            || expected == model_core::kImportRequestUsdExpectedUsdc
            || expected == model_core::kImportRequestUsdExpectedUsdz;
        if (!request.generationId || !request.sourceFileHandleValue
            || !request.sectionHandleValue || !request.cancellationEventHandleValue
            || request.sectionByteCapacity < sizeof(model_core::SectionHeader)
            || !request.maxChunkCount || !oneExpectedBit
            || (request.requestFlags & ~model_core::kImportRequestUsdExpectedMask)) {
            if (!SendError(request.generationId,
                           model_core::ImportErrorCode::CompatibilityHostFailure)) return 75;
            continue;
        }

        platform::Win32Handle source(reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(request.sourceFileHandleValue)));
        platform::Win32Handle cancellation(reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(request.cancellationEventHandleValue)));
        platform::Win32Handle output(reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(request.sectionHandleValue)));
        BY_HANDLE_FILE_INFORMATION sourceInfo{};
        auto outputView = platform::MappedView::Map(
            output.get(), FILE_MAP_READ | FILE_MAP_WRITE,
            static_cast<SIZE_T>(request.sectionByteCapacity));
        if (!GetFileInformationByHandle(source.get(), &sourceInfo) || !outputView) {
            if (!SendError(request.generationId,
                           model_core::ImportErrorCode::CompatibilityHostFailure)) return 76;
            continue;
        }
        if (WaitForSingleObject(cancellation.get(), 0) == WAIT_OBJECT_0) {
            if (!SendError(request.generationId, model_core::ImportErrorCode::Cancelled)) return 77;
            continue;
        }
        const HMODULE core = LoadAndAuditProductionPayload(directory);
        if (!core) {
            if (!SendError(request.generationId,
                           model_core::ImportErrorCode::CompatibilityHostFailure)) return 82;
            continue;
        }

        using RunFunction = int (__cdecl*)(
            const model_core::ParseOpenUsdFileRequest*, std::byte*, std::size_t,
            void*, void*, const wchar_t*, compatibility_host::OpenUsdImportResult*);
        const auto run = reinterpret_cast<RunFunction>(
            GetProcAddress(core, "Preview3DRunOpenUsdImport"));
        compatibility_host::OpenUsdImportResult result{};
        if (!run || run(&request, outputView.bytes().data(), outputView.bytes().size(),
                        GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE),
                        directory.c_str(), &result) != 0) {
            if (!SendError(request.generationId,
                           model_core::ImportErrorCode::CompatibilityHostFailure)) return 78;
            continue;
        }
        if (result.errorCode != model_core::ImportErrorCode::None) {
            model_core::GenerationErrorNotice notice{};
            notice.generationId = request.generationId;
            notice.errorCode = static_cast<std::uint32_t>(result.errorCode);
            notice.reserved0 = static_cast<std::uint32_t>(result.phase);
            if (!model_core::WriteControlMessage(
                    GetStdHandle(STD_OUTPUT_HANDLE), model_core::ControlOpcode::GenerationError,
                    &notice, sizeof(notice))) return 78;
            continue;
        }
        model_core::ChunksReadyNotice notice{};
        notice.generationId = request.generationId;
        notice.chunkCount = result.chunkCount;
        notice.sectionBytesWritten = result.sectionBytesWritten;
        if (!model_core::WriteControlMessage(
                GetStdHandle(STD_OUTPUT_HANDLE), model_core::ControlOpcode::ChunksReady,
                &notice, sizeof(notice))) return 78;
    }
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    using namespace compatibility_host;
    const auto directory = ExecutableDirectory();
    if (directory.empty() || !HardenProcessDiscovery(directory)) return 65;
    if (argc == 2) {
        const std::wstring_view mode(argv[1]);
        if (mode == L"--pool") return RunProductionPool(PoolMode::Normal, directory);
        if (mode == L"--pool-crash") return RunProductionPool(PoolMode::Crash, directory);
        if (mode == L"--pool-hang") return RunProductionPool(PoolMode::Hang, directory);
        if (mode == L"--pool-overallocate") return RunProductionPool(PoolMode::ConsumeMemory, directory);
        if (mode == L"--pool-stale") return RunProductionPool(PoolMode::StaleReply, directory);
        if (mode == L"--pool-reverse-fallback") return RunProductionPool(PoolMode::ReverseFallback, directory);
    }
    if (argc != 4 || std::wstring_view(argv[1]) != L"--usd-002-spike") return 64;

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
    return result;
}

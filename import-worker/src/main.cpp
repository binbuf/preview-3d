#include "ContainmentProbes.h"
#include "ChunkBatchSink.h"
#include "GenerationWorker.h"
#include "FbxSpikeWorker.h"
#include "ThreeMfSpikeWorker.h"
#include "UsdSpikeWorker.h"
#include "UsdImportWorker.h"
#include "ThreeMfImportWorker.h"
#include "FbxImportWorker.h"
#include "GltfImportWorker.h"
#include "PlyImportWorker.h"
#include "ObjImportWorker.h"
#include "StlImportWorker.h"
#include "WorkerRequestDispatch.h"
#include "model_core/ControlChannelIo.h"

#include <windows.h>

#include <objbase.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path ExecutableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

// Establish the safe loader/discovery state before any request is parsed, the
// same way both compatibility hosts do (see compatibility-host/src/main.cpp
// and compatibility-host-step/src/main.cpp). DLL resolution is restricted to
// System32 plus the explicit payload directory, the current directory is the
// payload directory, and every inherited plug-in/loader escape hatch is
// cleared. This is defense in depth: the broker also passes an explicit
// scrubbed environment and payload working directory to every child.
bool HardenProcessDiscovery(const std::filesystem::path& directory)
{
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS))
        return false;
    if (!AddDllDirectory(directory.c_str()) || !SetCurrentDirectoryW(directory.c_str()))
        return false;

    constexpr const wchar_t* variables[] = {
        // OpenUSD / USD imaging / MaterialX / RenderMan plug-in and search paths.
        L"PREVIEW3D_DISABLED_PLUGIN_PATH", L"PXR_PLUGINPATH_NAME",
        L"PXR_AR_DEFAULT_SEARCH_PATH", L"PYTHONPATH", L"USDIMAGING_ENABLE_PLUGINS",
        L"MATERIALX_SEARCH_PATH", L"RMANTREE", L"RMAN_RIXPLUGINPATH",
        // OCCT resource/plug-in configuration (shared with the STEP host so a
        // single scrub list covers every child).
        L"CSF_OCCTResourcePath", L"CSF_PluginPath", L"CSF_UnitsLexicon",
        L"CSF_DefaultUnit", L"CSF_UnitsDefinition", L"CSF_IGESDefaults",
        L"CSF_STEPDefaults", L"CSF_XCAFDefaults", L"CSF_DrawPluginPath",
        L"CSF_MDTVTexturesDirectory", L"CSF_ShadersDirectory",
        L"CSF_GraphicShr", L"MMGT_OPT", L"PATH"
    };
    for (const auto variable : variables) SetEnvironmentVariableW(variable, nullptr);
    return true;
}

std::wstring WidenUtf8(const char* text)
{
    if (text == nullptr || *text == '\0') {
        return {};
    }
    int required = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(required) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), required);
    return wide;
}

bool ArgEquals(const char* arg, const char* value)
{
    return std::strcmp(arg, value) == 0;
}

} // namespace

int main(int argc, char* argv[])
{
    // First COM usage anywhere in this process (confirmed zero prior usage
    // this session) -- WicImageDecodeAdapter.cpp needs IWICImagingFactory.
    // COINIT_MULTITHREADED rather than the COINIT_APARTMENTTHREADED
    // precedent Preview3D.cpp uses: this is a headless console process with
    // no message pump, so there's no OLE/message-loop semantics to match.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const auto directory = ExecutableDirectory();
    if (directory.empty() || !HardenProcessDiscovery(directory)) {
        return 1;
    }

    if (argc < 2) {
        return 1;
    }

#ifdef PREVIEW3D_ENABLE_FAULT_HARNESS
    if (ArgEquals(argv[1], "--hang")) {
        import_worker::RunHangProbe();
        return 0;
    }
    if (ArgEquals(argv[1], "--cpu-spin")) {
        import_worker::RunCpuSpinProbe();
        return 0;
    }
    if (ArgEquals(argv[1], "--test-hang-import")) {
        model_core::ReadControlMessage(GetStdHandle(STD_INPUT_HANDLE));
        Sleep(INFINITE); return 1;
    }
    if (ArgEquals(argv[1], "--test-invalid-import-reply")) {
        auto request = model_core::ReadControlMessage(GetStdHandle(STD_INPUT_HANDLE));
        if (!request || request->payload.size() < 8) return 1;
        model_core::GenerationErrorNotice notice{};
        std::memcpy(&notice.generationId,request->payload.data(),8); notice.errorCode=UINT32_MAX;
        model_core::WriteControlMessage(GetStdHandle(STD_OUTPUT_HANDLE),model_core::ControlOpcode::GenerationError,&notice,sizeof(notice));
        return 1;
    }

    if (ArgEquals(argv[1], "--overallocate")) {
        import_worker::RunOverallocateProbe();
        return 0;
    }

    if (ArgEquals(argv[1], "--child-noop")) {
        return 0;
    }
#endif // PREVIEW3D_ENABLE_FAULT_HARNESS

    if (ArgEquals(argv[1], "--generate")) {
        return import_worker::RunGeneration();
    }

#ifdef PREVIEW3D_ENABLE_FAULT_HARNESS
    if (ArgEquals(argv[1], "--parse-gltf-delayed-batches")) {
        import_worker::ChunkBatchSink::SetDelayForTesting(1500);
        return import_worker::RunGltfImport();
    }
#endif // PREVIEW3D_ENABLE_FAULT_HARNESS

    if (ArgEquals(argv[1], "--parse-gltf")) {
        return import_worker::RunGltfImport();
    }
    if (ArgEquals(argv[1], "--parse-gltf-proxy-detail")) {
        import_worker::ChunkBatchSink::EnableDetailService(); return import_worker::RunGltfImport();
    }
    if (ArgEquals(argv[1], "--parse-stl-proxy-detail")) {
        import_worker::ChunkBatchSink::EnableDetailService(); return import_worker::RunStlImport();
    }
    if (ArgEquals(argv[1], "--parse-ply-proxy-detail")) {
        import_worker::ChunkBatchSink::EnableDetailService(); return import_worker::RunPlyImport();
    }
    if (ArgEquals(argv[1], "--parse-gltf-proxy")) {
        import_worker::ChunkBatchSink::EnableCoarseProxy(); return import_worker::RunGltfImport();
    }
    if (ArgEquals(argv[1], "--parse-stl-proxy")) {
        import_worker::ChunkBatchSink::EnableCoarseProxy(); return import_worker::RunStlImport();
    }
    if (ArgEquals(argv[1], "--parse-ply-proxy")) {
        import_worker::ChunkBatchSink::EnableCoarseProxy(); return import_worker::RunPlyImport();
    }

    if (ArgEquals(argv[1], "--parse-stl")) {
        return import_worker::RunStlImport();
    }

    if (ArgEquals(argv[1], "--parse-ply")) {
        return import_worker::RunPlyImport();
    }
    if (ArgEquals(argv[1], "--parse-obj")) {
        return import_worker::RunObjImport();
    }
    if (ArgEquals(argv[1], "--parse-fbx")) {
        return import_worker::RunFbxImport();
    }
    if (ArgEquals(argv[1], "--parse-usd")) {
        return import_worker::RunUsdImport();
    }
    if (ArgEquals(argv[1], "--parse-3mf")) {
        return import_worker::RunThreeMfImport();
    }

    // Backward-compatible aliases retained for older parser regression commands.
#ifdef PREVIEW3D_ENABLE_FAULT_HARNESS
    if (ArgEquals(argv[1], "--test-parse-stl-ascii")) return import_worker::RunStlImport(true);
    if (ArgEquals(argv[1], "--test-parse-ply-ascii")) return import_worker::RunPlyImport(true);
#endif // PREVIEW3D_ENABLE_FAULT_HARNESS
    if (ArgEquals(argv[1], "--pool")) {
        return import_worker::RunPoolMode();
    }
    if (ArgEquals(argv[1], "--fbx-spike-pool")) {
        return import_worker::RunFbxSpikePoolMode();
    }
    if (ArgEquals(argv[1], "--usd-spike-pool")) {
        return import_worker::RunUsdSpikePoolMode();
    }
    if (ArgEquals(argv[1], "--3mf-spike-pool")) {
        return import_worker::RunThreeMfSpikePoolMode();
    }

    if (ArgEquals(argv[1], "--probes")) {
        if (argc < 4) {
            return 1;
        }

        std::wstring canaryPath = WidenUtf8(argv[2]);
        unsigned short port = static_cast<unsigned short>(std::atoi(argv[3]));

        import_worker::RunFilesystemEscapeProbe(canaryPath.c_str());
        import_worker::RunNetworkEscapeProbe(port);
        import_worker::RunProcessSpawnEscapeProbe();
        import_worker::ReportDone();
        return 0;
    }

    return 1;
}

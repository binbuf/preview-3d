#include "ContainmentProbes.h"
#include "GenerationWorker.h"
#include "GltfImportWorker.h"
#include "PlyImportWorker.h"
#include "StlImportWorker.h"
#include "WorkerRequestDispatch.h"

#include <windows.h>

#include <objbase.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

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

    if (argc < 2) {
        return 1;
    }

    if (ArgEquals(argv[1], "--hang")) {
        import_worker::RunHangProbe();
        return 0;
    }

    if (ArgEquals(argv[1], "--overallocate")) {
        import_worker::RunOverallocateProbe();
        return 0;
    }

    if (ArgEquals(argv[1], "--child-noop")) {
        return 0;
    }

    if (ArgEquals(argv[1], "--generate")) {
        return import_worker::RunGeneration();
    }

    if (ArgEquals(argv[1], "--parse-gltf")) {
        return import_worker::RunGltfImport();
    }

    if (ArgEquals(argv[1], "--parse-stl")) {
        return import_worker::RunStlImport();
    }

    if (ArgEquals(argv[1], "--parse-ply")) {
        return import_worker::RunPlyImport();
    }

    if (ArgEquals(argv[1], "--pool")) {
        return import_worker::RunPoolMode();
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

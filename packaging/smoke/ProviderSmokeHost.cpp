// T22 first installed Release smoke verifier.
//
// This is the developer/QA-local verification half of the smoke procedure: it
// is NOT the release artifact. It proves, on this machine, that
//
//   1. the built Preview3DThumbnailProvider.dll renders a real .stl through the
//      provider COM path (in-process reference image), and
//   2. the real Shell path (IThumbnailCache::GetThumbnail, the same path
//      Explorer uses) returns a thumbnail that matches that reference, so the
//      thumbnail is model-derived and produced by this provider, and
//   3. the handler loads out-of-process in the DllHost surrogate and not in the
//      caller (module-identity scan), with no DisableProcessIsolation opt-out.
//
// The reference image is rendered in-process by LoadLibrary-ing the staged DLL
// and calling its PRIVATE DllGetClassObject, because an HBITMAP cannot be
// marshaled across a COM surrogate boundary. The Shell image is obtained
// through IThumbnailCache, whose ISharedBitmap lives in this process. Comparing
// the two is what ties the Shell thumbnail to this provider.
//
// Usage:
//   ProviderSmokeHost.exe --dll <path> --stl <path> [--cx 256] [--out <dir>]
//
// Exit code 0 means every check passed; 1 means a check failed; 2 is a usage or
// setup error. It writes reference.pam, shell.pam and report.txt into --out.

#include <windows.h>

#include <initguid.h>
#include <objbase.h>
#include <objidl.h>
#include <propsys.h>
#include <psapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

const CLSID kStlClsid = {0xBFC86E1A, 0x55C1, 0x4C2D, {0xAA, 0x36, 0x3C, 0x25, 0xDE, 0xCF, 0x30, 0xC9}};
const CLSID kPlyClsid = {0xF4DC6119, 0xE235, 0x4BAC, {0x80, 0x89, 0x54, 0xED, 0xD8, 0x4F, 0x84, 0x92}};
const CLSID kGltfClsid = {0xA592F425, 0xEA68, 0x4C88, {0xBB, 0x96, 0x02, 0x08, 0x05, 0xD4, 0xBE, 0x56}};
const CLSID kFbxClsid = {0xFBC218D4, 0xFD2C, 0x41DF, {0xB1, 0x68, 0x7F, 0x3B, 0x9E, 0x53, 0xC8, 0x4E}};
const CLSID kThreeMfClsid = {0xD8389A63, 0x8526, 0x454A, {0x98, 0x92, 0x72, 0xF3, 0x14, 0x94, 0x84, 0xB9}};
constexpr wchar_t kProviderModuleName[] = L"Preview3DThumbnailProvider.dll";
constexpr wchar_t kStlClsidKey[] = L"Software\\Classes\\CLSID\\{BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9}";
constexpr wchar_t kStlAppIdKey[] = L"Software\\Classes\\AppID\\{BFC86E1A-55C1-4C2D-AA36-3C25DECF3010}";
constexpr wchar_t kPlyClsidKey[] = L"Software\\Classes\\CLSID\\{F4DC6119-E235-4BAC-8089-54EDD84F8492}";
constexpr wchar_t kPlyAppIdKey[] = L"Software\\Classes\\AppID\\{F4DC6119-E235-4BAC-8089-54EDD84F8493}";
constexpr wchar_t kGltfClsidKey[] = L"Software\\Classes\\CLSID\\{A592F425-EA68-4C88-BB96-020805D4BE56}";
constexpr wchar_t kGltfAppIdKey[] = L"Software\\Classes\\AppID\\{A592F425-EA68-4C88-BB96-020805D4BE57}";
constexpr wchar_t kFbxClsidKey[] = L"Software\\Classes\\CLSID\\{FBC218D4-FD2C-41DF-B168-7F3B9E53C84E}";
constexpr wchar_t kFbxAppIdKey[] = L"Software\\Classes\\AppID\\{FBC218D4-FD2C-41DF-B168-7F3B9E53C84F}";
constexpr wchar_t kThreeMfClsidKey[] = L"Software\\Classes\\CLSID\\{D8389A63-8526-454A-9892-72F3149484B9}";
constexpr wchar_t kThreeMfAppIdKey[] = L"Software\\Classes\\AppID\\{D8389A63-8526-454A-9892-72F3149484BA}";

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}

std::string GuidToText(const GUID& guid)
{
    wchar_t buffer[64]{};
    if (StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer))) <= 0) return {};
    return ToUtf8(buffer);
}

bool RegistryValuePresent(HKEY root, const wchar_t* subKey, const wchar_t* valueName)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD size = 0;
    const bool present = RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    return present;
}

struct ModuleHit {
    DWORD pid = 0;
    std::wstring process;
    std::wstring modulePath;
};

// Module-identity method (T03): enumerate every process's loaded modules and
// report the processes that have `moduleName` mapped.
std::vector<ModuleHit> ScanForModule(const wchar_t* moduleName)
{
    std::vector<ModuleHit> hits;
    const HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes == INVALID_HANDLE_VALUE) return hits;
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    if (Process32FirstW(processes, &process)) {
        do {
            const HANDLE modules =
                CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process.th32ProcessID);
            if (modules == INVALID_HANDLE_VALUE) continue;
            MODULEENTRY32W module{};
            module.dwSize = sizeof(module);
            if (Module32FirstW(modules, &module)) {
                do {
                    if (_wcsicmp(module.szModule, moduleName) == 0) {
                        hits.push_back({process.th32ProcessID, process.szExeFile, module.szExePath});
                        break;
                    }
                } while (Module32NextW(modules, &module));
            }
            CloseHandle(modules);
        } while (Process32NextW(processes, &process));
    }
    CloseHandle(processes);
    return hits;
}

std::wstring DescribeHits(const std::vector<ModuleHit>& hits)
{
    std::wstring text;
    for (const ModuleHit& hit : hits) {
        if (!text.empty()) text += L", ";
        text += std::to_wstring(hit.pid);
        text += L":";
        text += hit.process;
    }
    return text.empty() ? L"none" : text;
}

std::wstring DescribeHitsDetailed(const std::vector<ModuleHit>& hits)
{
    std::wstring text;
    for (const ModuleHit& hit : hits) {
        if (!text.empty()) text += L" | ";
        text += std::to_wstring(hit.pid);
        text += L":" + hit.process + L":" + hit.modulePath;
    }
    return text.empty() ? L"none" : text;
}

bool IsDllHost(const ModuleHit& hit)
{
    return _wcsicmp(hit.process.c_str(), L"dllhost.exe") == 0;
}

bool BitmapToRgba(HBITMAP bitmap, int& width, int& height, std::vector<std::uint8_t>& rgba)
{
    if (bitmap == nullptr) return false;
    BITMAP info{};
    if (GetObjectW(bitmap, sizeof(info), &info) == 0 || info.bmWidth <= 0 || info.bmHeight <= 0) {
        return false;
    }
    width = info.bmWidth;
    height = info.bmHeight;

    BITMAPINFO dib{};
    dib.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dib.bmiHeader.biWidth = width;
    dib.bmiHeader.biHeight = -height; // top-down
    dib.bmiHeader.biPlanes = 1;
    dib.bmiHeader.biBitCount = 32;
    dib.bmiHeader.biCompression = BI_RGB;

    std::vector<std::uint8_t> bgra(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    const HDC dc = GetDC(nullptr);
    const int copied = GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), bgra.data(), &dib, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (copied == 0) return false;

    rgba.resize(bgra.size());
    for (std::size_t i = 0; i < bgra.size(); i += 4) {
        rgba[i + 0] = bgra[i + 2];
        rgba[i + 1] = bgra[i + 1];
        rgba[i + 2] = bgra[i + 0];
        rgba[i + 3] = bgra[i + 3];
    }
    return true;
}

bool SavePam(const std::wstring& path, int width, int height, const std::vector<std::uint8_t>& rgba)
{
    FILE* file = nullptr;
    if (fopen_s(&file, ToUtf8(path).c_str(), "wb") != 0 || file == nullptr) return false;
    std::fprintf(file, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", width, height);
    const std::size_t written = rgba.empty() ? 0 : std::fwrite(rgba.data(), 1, rgba.size(), file);
    std::fclose(file);
    return written == rgba.size();
}

struct ImageStats {
    double opaqueFraction = 0.0;
    int maxChannel = 0;
};

ImageStats MeasureRgba(const std::vector<std::uint8_t>& rgba)
{
    ImageStats stats;
    if (rgba.empty()) return stats;
    std::size_t opaque = 0;
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        if (rgba[i + 3] != 0) ++opaque;
        stats.maxChannel = (std::max)(stats.maxChannel, static_cast<int>(rgba[i + 0]));
        stats.maxChannel = (std::max)(stats.maxChannel, static_cast<int>(rgba[i + 1]));
        stats.maxChannel = (std::max)(stats.maxChannel, static_cast<int>(rgba[i + 2]));
    }
    stats.opaqueFraction = static_cast<double>(opaque) / static_cast<double>(rgba.size() / 4);
    return stats;
}

struct ImageDiff {
    bool valid = false;
    double meanAbs = 0.0;
    int maxAbs = 0;
};

ImageDiff CompareRgba(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b)
{
    ImageDiff diff;
    if (a.size() != b.size() || a.empty()) return diff;
    diff.valid = true;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const int delta = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
        total += static_cast<std::uint64_t>(delta);
        diff.maxAbs = (std::max)(diff.maxAbs, delta);
    }
    diff.meanAbs = static_cast<double>(total) / static_cast<double>(a.size());
    return diff;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::wstring dllPath;
    std::wstring stlPath;
    std::wstring plyPath;
    std::wstring gltfPath;
    std::wstring fbxPath;
    std::wstring mfPath;
    std::wstring outDir;
    unsigned cx = 256;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring { return (i + 1 < argc) ? argv[++i] : std::wstring(); };
        if (arg == L"--dll") dllPath = next();
        else if (arg == L"--stl") stlPath = next();
        else if (arg == L"--ply") plyPath = next();
        else if (arg == L"--gltf") gltfPath = next();
        else if (arg == L"--fbx") fbxPath = next();
        else if (arg == L"--mf") mfPath = next();
        else if (arg == L"--out") outDir = next();
        else if (arg == L"--cx") cx = static_cast<unsigned>(_wtoi(next().c_str()));
    }
    if (dllPath.empty() || (stlPath.empty() && plyPath.empty() && gltfPath.empty() && fbxPath.empty()
                            && mfPath.empty())) {
        std::printf("usage: ProviderSmokeHost.exe --dll <path> (--stl <path> | --ply <path> | --gltf <path> | --fbx <path> | --mf <path>) [--cx 256] [--out <dir>]\n");
        return 2;
    }
    if (outDir.empty()) outDir = L".";

    // Each family smoke selects its own frozen CLSID and AppID; the rest of the
    // procedure (surrogate hosting, Shell path, image match) is family-agnostic.
    enum class SmokeFamily { Stl, Ply, Gltf, Fbx, ThreeMf };
    SmokeFamily family = SmokeFamily::Stl;
    const std::wstring* model = &stlPath;
    if (!gltfPath.empty()) {
        family = SmokeFamily::Gltf;
        model = &gltfPath;
    } else if (!fbxPath.empty()) {
        family = SmokeFamily::Fbx;
        model = &fbxPath;
    } else if (!mfPath.empty()) {
        family = SmokeFamily::ThreeMf;
        model = &mfPath;
    } else if (!plyPath.empty()) {
        family = SmokeFamily::Ply;
        model = &plyPath;
    }
    const std::wstring& modelPath = *model;
    const CLSID clsid = family == SmokeFamily::Gltf ? kGltfClsid
        : (family == SmokeFamily::Fbx ? kFbxClsid
           : (family == SmokeFamily::ThreeMf ? kThreeMfClsid
              : (family == SmokeFamily::Ply ? kPlyClsid : kStlClsid)));
    const wchar_t* clsidKey = family == SmokeFamily::Gltf ? kGltfClsidKey
        : (family == SmokeFamily::Fbx ? kFbxClsidKey
           : (family == SmokeFamily::ThreeMf ? kThreeMfClsidKey
              : (family == SmokeFamily::Ply ? kPlyClsidKey : kStlClsidKey)));
    const wchar_t* appIdKey = family == SmokeFamily::Gltf ? kGltfAppIdKey
        : (family == SmokeFamily::Fbx ? kFbxAppIdKey
           : (family == SmokeFamily::ThreeMf ? kThreeMfAppIdKey
              : (family == SmokeFamily::Ply ? kPlyAppIdKey : kStlAppIdKey)));
    const char* familyName = family == SmokeFamily::Gltf ? "gltf"
        : (family == SmokeFamily::Fbx ? "fbx"
           : (family == SmokeFamily::ThreeMf ? "3mf"
              : (family == SmokeFamily::Ply ? "ply" : "stl")));

    FILE* report = nullptr;
    if (fopen_s(&report, ToUtf8(outDir + L"\\report.txt").c_str(), "wb") != 0) report = nullptr;
    auto say = [&](const char* text) {
        std::printf("%s\n", text);
        if (report != nullptr) std::fprintf(report, "%s\n", text);
    };
    auto closeReport = [&]() {
        if (report != nullptr) {
            std::fclose(report);
            report = nullptr;
        }
    };
    auto fail = [&](const char* message) {
        std::printf("FAIL: %s\n", message);
        if (report != nullptr) std::fprintf(report, "FAIL: %s\n", message);
        closeReport();
        return 1;
    };

    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initHr)) return fail("CoInitializeEx failed");

    std::printf("provider smoke host\n");
    std::printf("  dll=%s\n", ToUtf8(dllPath).c_str());
    std::printf("  %s=%s\n", familyName, ToUtf8(modelPath).c_str());
    std::printf("  cx=%u out=%s\n", cx, ToUtf8(outDir).c_str());
    std::printf("  %s clsid=%s\n", familyName, GuidToText(clsid).c_str());
    if (report != nullptr) {
        std::fprintf(report, "provider smoke report\n");
        std::fprintf(report, "dll=%s\n", ToUtf8(dllPath).c_str());
        std::fprintf(report, "model=%s\n", ToUtf8(modelPath).c_str());
        std::fprintf(report, "family=%s\n", familyName);
        std::fprintf(report, "cx=%u\n", cx);
        std::fprintf(report, "clsid=%s\n", GuidToText(clsid).c_str());
    }

    // -- 1. no DisableProcessIsolation anywhere we register ------------------
    const bool clsidOptOutHkcu = RegistryValuePresent(HKEY_CURRENT_USER, clsidKey, L"DisableProcessIsolation");
    const bool clsidOptOutHklm = RegistryValuePresent(HKEY_LOCAL_MACHINE, clsidKey, L"DisableProcessIsolation");
    const bool appIdOptOutHkcu = RegistryValuePresent(HKEY_CURRENT_USER, appIdKey, L"DisableProcessIsolation");
    const bool appIdOptOutHklm = RegistryValuePresent(HKEY_LOCAL_MACHINE, appIdKey, L"DisableProcessIsolation");
    const bool anyOptOut = clsidOptOutHkcu || clsidOptOutHklm || appIdOptOutHkcu || appIdOptOutHklm;
    std::printf("  DisableProcessIsolation: HKCU_CLSID=%s HKLM_CLSID=%s HKCU_AppID=%s HKLM_AppID=%s\n",
                clsidOptOutHkcu ? "present" : "absent", clsidOptOutHklm ? "present" : "absent",
                appIdOptOutHkcu ? "present" : "absent", appIdOptOutHklm ? "present" : "absent");
    if (report != nullptr) {
        std::fprintf(report, "disable_process_isolation_present=%s\n", anyOptOut ? "true" : "false");
    }

    // -- 2. in-process reference render through the provider's PRIVATE exports -
    std::vector<std::uint8_t> reference;
    int referenceWidth = 0;
    int referenceHeight = 0;
    HRESULT renderHr = E_FAIL;
    bool referenceOk = false;
    HMODULE module = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        CoUninitialize();
        return fail("could not load the staged provider DLL");
    }
    {
        using GetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
        auto getClassObject = reinterpret_cast<GetClassObjectFn>(GetProcAddress(module, "DllGetClassObject"));
        IStream* stream = nullptr;
        if (getClassObject == nullptr) {
            renderHr = E_FAIL;
        } else if (FAILED(SHCreateStreamOnFileEx(modelPath.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
                                                 FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream))) {
            renderHr = E_FAIL;
        } else {
            void* raw = nullptr;
            renderHr = getClassObject(clsid, __uuidof(IClassFactory), &raw);
            if (SUCCEEDED(renderHr) && raw != nullptr) {
                auto* factory = static_cast<IClassFactory*>(raw);
                IThumbnailProvider* provider = nullptr;
                renderHr = factory->CreateInstance(nullptr, __uuidof(IThumbnailProvider),
                                                   reinterpret_cast<void**>(&provider));
                factory->Release();
                if (SUCCEEDED(renderHr) && provider != nullptr) {
                    IInitializeWithStream* init = nullptr;
                    renderHr = provider->QueryInterface(__uuidof(IInitializeWithStream),
                                                        reinterpret_cast<void**>(&init));
                    if (SUCCEEDED(renderHr) && init != nullptr) {
                        renderHr = init->Initialize(stream, STGM_READ);
                        if (SUCCEEDED(renderHr)) {
                            HBITMAP bitmap = nullptr;
                            WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
                            renderHr = provider->GetThumbnail(cx, &bitmap, &alpha);
                            if (SUCCEEDED(renderHr) && bitmap != nullptr) {
                                referenceOk = BitmapToRgba(bitmap, referenceWidth, referenceHeight, reference);
                                if (referenceOk) SavePam(outDir + L"\\reference.pam", referenceWidth, referenceHeight,
                                                         reference);
                                DeleteObject(bitmap);
                            }
                        }
                        init->Release();
                    }
                    provider->Release();
                }
            }
            stream->Release();
        }
    }
    FreeLibrary(module);
    module = nullptr;
    if (!referenceOk) {
        std::printf("  reference render hr=0x%08lX\n", static_cast<unsigned long>(renderHr));
        CoUninitialize();
        return fail("in-process reference render failed");
    }
    const ImageStats referenceStats = MeasureRgba(reference);
    std::printf("  reference bitmap=%dx%d opaque=%.4f maxChannel=%d\n", referenceWidth, referenceHeight,
                referenceStats.opaqueFraction, referenceStats.maxChannel);
    if (report != nullptr) {
        std::fprintf(report, "reference=%dx%d opaque_fraction=%.4f\n", referenceWidth, referenceHeight,
                     referenceStats.opaqueFraction);
    }

    // -- 3. handler must not stay loaded in this process ---------------------
    const bool selfLoaded = GetModuleHandleW(kProviderModuleName) != nullptr;
    std::printf("  provider loaded in this host process: %s\n", selfLoaded ? "YES" : "no");
    if (report != nullptr) std::fprintf(report, "loaded_in_host=%s\n", selfLoaded ? "true" : "false");

    // -- 4. explicit surrogate activation (AppID + DllSurrogate) --------------
    bool surrogateObserved = false;
    IThumbnailProvider* remoteProvider = nullptr;
    const HRESULT remoteHr =
        CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, __uuidof(IThumbnailProvider),
                         reinterpret_cast<void**>(&remoteProvider));
    std::vector<ModuleHit> afterActivation;
    if (SUCCEEDED(remoteHr) && remoteProvider != nullptr) {
        afterActivation = ScanForModule(kProviderModuleName);
        for (const ModuleHit& hit : afterActivation) {
            if (IsDllHost(hit)) surrogateObserved = true;
        }
        remoteProvider->Release();
    }
    std::printf("  CLSCTX_LOCAL_SERVER hr=0x%08lX; module hosts: %s\n", static_cast<unsigned long>(remoteHr),
                ToUtf8(DescribeHits(afterActivation)).c_str());
    if (report != nullptr) {
        std::fprintf(report, "local_server_hr=0x%08lX\n", static_cast<unsigned long>(remoteHr));
        std::fprintf(report, "module_hosts_after_activation=%s\n",
                     ToUtf8(DescribeHitsDetailed(afterActivation)).c_str());
    }

    // -- 5. real Shell path: IThumbnailCache::GetThumbnail --------------------
    std::vector<std::uint8_t> shell;
    int shellWidth = 0;
    int shellHeight = 0;
    WTS_ALPHATYPE shellAlpha = WTSAT_UNKNOWN;
    WTS_CACHEFLAGS shellFlags = WTS_DEFAULT;
    bool shellOk = false;
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(modelPath.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        CoUninitialize();
        return fail("SHCreateItemFromParsingName failed");
    }
    IThumbnailCache* cache = nullptr;
    HRESULT shellHr =
        CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cache));
    if (SUCCEEDED(shellHr) && cache != nullptr) {
        ISharedBitmap* shared = nullptr;
        WTS_THUMBNAILID id{};
        shellHr = cache->GetThumbnail(item, cx, static_cast<WTS_FLAGS>(WTS_EXTRACT | WTS_FORCEEXTRACTION), &shared,
                                      &shellFlags, &id);
        if (SUCCEEDED(shellHr) && shared != nullptr) {
            shared->GetFormat(&shellAlpha);
            HBITMAP bitmap = nullptr;
            if (SUCCEEDED(shared->GetSharedBitmap(&bitmap)) && bitmap != nullptr) {
                shellOk = BitmapToRgba(bitmap, shellWidth, shellHeight, shell);
                if (shellOk) SavePam(outDir + L"\\shell.pam", shellWidth, shellHeight, shell);
                DeleteObject(bitmap);
            }
            shared->Release();
        }
        cache->Release();
    }
    item->Release();
    const std::vector<ModuleHit> afterShell = ScanForModule(kProviderModuleName);
    for (const ModuleHit& hit : afterShell) {
        if (IsDllHost(hit)) surrogateObserved = true;
    }
    if (!shellOk) {
        std::printf("  Shell GetThumbnail hr=0x%08lX\n", static_cast<unsigned long>(shellHr));
        CoUninitialize();
        return fail("real Shell IThumbnailCache::GetThumbnail returned no bitmap");
    }
    const ImageStats shellStats = MeasureRgba(shell);
    std::printf("  shell thumbnail=%dx%d alpha=%d flags=0x%08lX opaque=%.4f; module hosts: %s\n", shellWidth,
                shellHeight, static_cast<int>(shellAlpha), static_cast<unsigned long>(shellFlags),
                shellStats.opaqueFraction, ToUtf8(DescribeHits(afterShell)).c_str());
    if (report != nullptr) {
        std::fprintf(report, "shell=%dx%d alpha=%d opaque_fraction=%.4f\n", shellWidth, shellHeight,
                     static_cast<int>(shellAlpha), shellStats.opaqueFraction);
        std::fprintf(report, "module_hosts_after_shell=%s\n", ToUtf8(DescribeHitsDetailed(afterShell)).c_str());
    }

    // -- 6. the Shell image must match the provider reference ----------------
    const bool sameSize = (referenceWidth == shellWidth && referenceHeight == shellHeight);
    const ImageDiff diff = sameSize ? CompareRgba(reference, shell) : ImageDiff{};
    const bool imageMatches = sameSize && diff.valid && diff.meanAbs <= 4.0 && diff.maxAbs <= 64;
    std::printf("  reference-vs-shell: sizes %s, meanAbs=%.4f maxAbs=%d -> %s\n", sameSize ? "match" : "differ",
                diff.meanAbs, diff.maxAbs, imageMatches ? "MATCH" : "DIFFER");
    if (report != nullptr) {
        std::fprintf(report, "sizes_match=%s\n", sameSize ? "true" : "false");
        std::fprintf(report, "mean_abs=%.4f\nmax_abs=%d\n", diff.meanAbs, diff.maxAbs);
        std::fprintf(report, "image_matches_reference=%s\n", imageMatches ? "true" : "false");
        std::fprintf(report, "surrogate_observed_in_dllhost=%s\n", surrogateObserved ? "true" : "false");
        std::fprintf(report, "isolation_opt_out_absent=%s\n", anyOptOut ? "false" : "true");
    }

    CoUninitialize();

    if (anyOptOut) return fail("a DisableProcessIsolation value is present");
    if (selfLoaded) return fail("the provider DLL is still loaded in the smoke host");
    if (!surrogateObserved) return fail("the provider was never observed in a DllHost surrogate");
    if (!imageMatches) return fail("the Shell thumbnail does not match the provider reference");
    say("PASS: surrogate hosting, no isolation opt-out, model-derived Shell thumbnail");
    closeReport();
    return 0;
}
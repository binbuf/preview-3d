// SPIKE-8b (T03) throwaway Shell-surrogate probe provider.
//
// A deliberately minimal COM in-proc server implementing IInitializeWithStream +
// IThumbnailProvider so T03 can prove that the Shell loads a thumbnail handler
// out-of-process (DllHost.exe surrogate) and can measure the T02 rasterizer
// inside that surrogate. This is NOT product code and is unregistered at the end
// of the spike.
//
// Modes (read from HKCU\Software\Preview3DThumbnailSpike\Mode):
//   control - solid teal image; proves the COM/bitmap plumbing alone
//   mesh    - T02 cap-sized mesh scene (2M inspected / 250k rasterized)
//   points  - T02 cap-sized point cloud (6M inspected / 250k rasterized)
//   gltf    - bounded compressed-glTF decode (draco/meshopt geometry plus
//             KTX2/Basis and WebP images) of HKCU\...\GltfPath, rendered with
//             the same T02 rasterizer. Forces the ADR-0003 decoder closure to
//             load and run inside the surrogate.
//   image   - decodes a standalone .ktx2/.webp from HKCU\...\GltfPath via the
//             same decoders and renders the control image, measuring the
//             image-decoder closure without a glTF container.
//
// Every GetThumbnail appends one line (with the host's RunToken) to the log so
// the host can prove which process actually executed the call.

#include <windows.h>

#include <shlobj.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <propsys.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "GltfSpikeDecode.h"
#include "SceneFixtures.h"
#include "ThumbnailRasterizer.h"

namespace
{

const CLSID kProbeClsid = {0xC7A5B3E1, 0x9D24, 0x4F88, {0xA1, 0xB6, 0x2E, 0x5C, 0x7D, 0x9F, 0x0A, 0x31}};

constexpr wchar_t kConfigKey[] = L"Software\\Preview3DThumbnailSpike";

HINSTANCE g_module = nullptr;
LONG g_objectCount = 0;
LONG g_lockCount = 0;

double NowMs()
{
    static LARGE_INTEGER frequency{};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return (static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart)) * 1000.0;
}

std::wstring ReadConfigString(const wchar_t* name, const wchar_t* fallback)
{
    wchar_t buffer[1024]{};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kConfigKey, name, RRF_RT_REG_SZ, &type, buffer, &size) == ERROR_SUCCESS) {
        return buffer;
    }
    return fallback;
}

std::wstring ToWide(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::wstring ProcessImagePath()
{
    wchar_t path[MAX_PATH * 4]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    if (!QueryFullProcessImageNameW(GetCurrentProcess(), 0, path, &length)) return L"<unknown>";
    return path;
}

std::wstring ModuleImagePath()
{
    wchar_t path[MAX_PATH * 4]{};
    const DWORD length = GetModuleFileNameW(g_module, path, static_cast<DWORD>(std::size(path)));
    if (length == 0) return L"<unknown>";
    return path;
}

std::uint64_t PrivateBytes()
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return pmc.PrivateUsage;
    }
    return 0;
}

std::uint64_t PeakCommit()
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return pmc.PeakPagefileUsage;
    }
    return 0;
}

void AppendLog(const std::wstring& line)
{
    const std::wstring path = ReadConfigString(L"LogPath", L"");
    if (path.empty()) return;
    const int length = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr,
                                           nullptr);
    std::string bytes(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), bytes.data(), length, nullptr, nullptr);
    bytes += "\r\n";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
}

HBITMAP CreateBgraBitmap(const std::vector<std::uint8_t>& bgra, int width, int height)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) return nullptr;
    const std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    if (bgra.size() < bytes) {
        DeleteObject(bitmap);
        return nullptr;
    }
    std::memcpy(bits, bgra.data(), bytes);
    return bitmap;
}

void FillControlImage(int size, std::vector<std::uint8_t>& out)
{
    out.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4u, 0);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)) * 4u;
            out[i + 0] = 0x90; // B (premultiplied, opaque)
            out[i + 1] = 0xB0; // G
            out[i + 2] = 0x40; // R
            out[i + 3] = 0xFF; // A
        }
    }
}

class ProbeThumbnailProvider final : public IInitializeWithStream, public IThumbnailProvider
{
public:
    ProbeThumbnailProvider() { InterlockedIncrement(&g_objectCount); }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (ppvObject == nullptr) return E_POINTER;
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IInitializeWithStream)) {
            *ppvObject = static_cast<IInitializeWithStream*>(this);
        } else if (riid == __uuidof(IThumbnailProvider)) {
            *ppvObject = static_cast<IThumbnailProvider*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refCount_)); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG remaining = InterlockedDecrement(&refCount_);
        if (remaining == 0) delete this;
        return static_cast<ULONG>(remaining);
    }

    // IInitializeWithStream
    HRESULT STDMETHODCALLTYPE Initialize(IStream* pstream, DWORD grfMode) override
    {
        (void)grfMode;
        if (pstream == nullptr) return E_POINTER;
        if (stream_ != nullptr) return E_UNEXPECTED;
        stream_ = pstream;
        stream_->AddRef();
        mode_ = ReadConfigString(L"Mode", L"control");
        token_ = ReadConfigString(L"RunToken", L"<none>");
        baselinePrivate_ = PrivateBytes();
        AppendLog(L"event=initialize pid=" + std::to_wstring(GetCurrentProcessId()) + L" process=" + ProcessImagePath() +
                  L" module=" + ModuleImagePath() + L" mode=" + mode_ + L" token=" + token_ + L" baseline_commit_bytes=" +
                  std::to_wstring(baselinePrivate_));
        return S_OK;
    }

    // IThumbnailProvider
    HRESULT STDMETHODCALLTYPE GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override
    {
        if (phbmp == nullptr || pdwAlpha == nullptr) return E_POINTER;
        *phbmp = nullptr;
        *pdwAlpha = WTSAT_ARGB;
        if (stream_ == nullptr) return E_UNEXPECTED;

        const double start = NowMs();
        const int size = static_cast<int>(cx > 512u ? 512u : cx);
        std::vector<std::uint8_t> pixels;
        std::uint64_t inspected = 0;
        std::uint64_t kept = 0;
        std::wstring extra;
        thumbnail_rasterizer::Status status = thumbnail_rasterizer::Status::Ok;

        if (mode_ == L"gltf") {
            const std::wstring gltfPath = ReadConfigString(L"GltfPath", L"");
            std::vector<thumbnail_rasterizer::Triangle> decoded;
            gltf_spike::DecodeStats gltfStats;
            std::string gltfError;
            if (gltfPath.empty() || !gltf_spike::Decode(gltfPath, decoded, gltfStats, gltfError)) {
                status = thumbnail_rasterizer::Status::NoGeometry;
                extra = L" gltf_error=" + ToWide(gltfError.empty() ? std::string("no path") : gltfError);
            } else {
                thumbnail_rasterizer::GeometryView view;
                view.triangles = decoded;
                thumbnail_rasterizer::Options options;
                options.size = size;
                options.supersample = 1;
                thumbnail_rasterizer::Image image;
                status = thumbnail_rasterizer::Render(view, options, image);
                pixels = std::move(image.bgraPremultiplied);
                inspected = gltfStats.vertices;
                kept = gltfStats.triangles;
                extra = L" decoder=" + ToWide(gltfStats.geometryDecoder) + L" source_bytes=" +
                        std::to_wstring(gltfStats.sourceBytes) + L" image_pixels=" +
                        std::to_wstring(gltfStats.imagePixels) + L" images=";
                for (std::size_t i = 0; i < gltfStats.decodedImages.size(); ++i) {
                    if (i != 0) extra += L",";
                    extra += ToWide(gltfStats.decodedImages[i]);
                }
            }
        } else if (mode_ == L"image") {
            const std::wstring imagePath = ReadConfigString(L"GltfPath", L"");
            gltf_spike::DecodeStats imageStats;
            std::string imageError;
            if (imagePath.empty() || !gltf_spike::DecodeStandaloneImage(imagePath, imageStats, imageError)) {
                status = thumbnail_rasterizer::Status::NoGeometry;
                extra = L" image_error=" + ToWide(imageError.empty() ? std::string("no path") : imageError);
            } else {
                FillControlImage(size, pixels);
                extra = L" decoder=" + ToWide(imageStats.decodedImages.empty() ? std::string("none")
                                                                               : imageStats.decodedImages[0]) +
                        L" source_bytes=" + std::to_wstring(imageStats.sourceBytes) + L" image_pixels=" +
                        std::to_wstring(imageStats.imagePixels);
            }
        } else if (mode_ == L"mesh") {
            auto scene = thumbnail_spike::GenerateLargeMesh(2000000, 250000, thumbnail_spike::SeedFromString("spike8-mesh"));
            inspected = scene.inspected;
            kept = scene.triangles.size();
            thumbnail_rasterizer::GeometryView view;
            view.triangles = scene.triangles;
            thumbnail_rasterizer::Options options;
            options.size = size;
            options.supersample = 1; // surrogate call already near the 750 ms p95 budget at the cap
            thumbnail_rasterizer::Image image;
            status = thumbnail_rasterizer::Render(view, options, image);
            pixels = std::move(image.bgraPremultiplied);
        } else if (mode_ == L"points") {
            auto scene = thumbnail_spike::GenerateLargePointCloud(6000000, 250000, thumbnail_spike::SeedFromString("spike8-points"));
            inspected = scene.inspected;
            kept = scene.points.size();
            thumbnail_rasterizer::GeometryView view;
            view.points = scene.points;
            thumbnail_rasterizer::Options options;
            options.size = size;
            options.supersample = 1;
            thumbnail_rasterizer::Image image;
            status = thumbnail_rasterizer::Render(view, options, image);
            pixels = std::move(image.bgraPremultiplied);
        } else {
            FillControlImage(size, pixels);
        }

        HBITMAP bitmap = status == thumbnail_rasterizer::Status::Ok ? CreateBgraBitmap(pixels, size, size) : nullptr;
        const double elapsed = NowMs() - start;
        const std::uint64_t after = PrivateBytes();
        const std::uint64_t peak = PeakCommit();

        AppendLog(L"event=thumbnail pid=" + std::to_wstring(GetCurrentProcessId()) + L" process=" + ProcessImagePath() +
                  L" token=" + token_ + L" requested_cx=" + std::to_wstring(cx) + L" rendered_size=" + std::to_wstring(size) +
                  L" mode=" + mode_ + L" inspected=" + std::to_wstring(inspected) + L" kept=" + std::to_wstring(kept) +
                  L" status=" + std::to_wstring(static_cast<int>(status)) + L" elapsed_ms=" + std::to_wstring(elapsed) +
                  L" commit_after_bytes=" + std::to_wstring(after) + L" commit_delta_bytes=" + std::to_wstring(after - baselinePrivate_) +
                  L" peak_bytes=" + std::to_wstring(peak) + extra);

        if (bitmap == nullptr) return E_FAIL;
        *phbmp = bitmap;
        return S_OK;
    }

private:
    ~ProbeThumbnailProvider()
    {
        if (stream_ != nullptr) stream_->Release();
        InterlockedDecrement(&g_objectCount);
    }

    LONG refCount_ = 1;
    IStream* stream_ = nullptr;
    std::wstring mode_ = L"control";
    std::wstring token_ = L"<none>";
    std::uint64_t baselinePrivate_ = 0;
};

class ProbeClassFactory final : public IClassFactory
{
public:
    ProbeClassFactory() { InterlockedIncrement(&g_objectCount); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (ppvObject == nullptr) return E_POINTER;
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppvObject = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refCount_)); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG remaining = InterlockedDecrement(&refCount_);
        if (remaining == 0) delete this;
        return static_cast<ULONG>(remaining);
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override
    {
        if (ppvObject == nullptr) return E_POINTER;
        *ppvObject = nullptr;
        if (pUnkOuter != nullptr) return CLASS_E_NOAGGREGATION;
        ProbeThumbnailProvider* provider = new (std::nothrow) ProbeThumbnailProvider();
        if (provider == nullptr) return E_OUTOFMEMORY;
        const HRESULT hr = provider->QueryInterface(riid, ppvObject);
        provider->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override
    {
        if (fLock) {
            InterlockedIncrement(&g_lockCount);
        } else {
            InterlockedDecrement(&g_lockCount);
        }
        return S_OK;
    }

private:
    ~ProbeClassFactory() { InterlockedDecrement(&g_objectCount); }
    LONG refCount_ = 1;
};

} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, kProbeClsid)) return CLASS_E_CLASSNOTAVAILABLE;
    ProbeClassFactory* factory = new (std::nothrow) ProbeClassFactory();
    if (factory == nullptr) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

extern "C" HRESULT WINAPI DllCanUnloadNow()
{
    return (g_objectCount == 0 && g_lockCount == 0) ? S_OK : S_FALSE;
}
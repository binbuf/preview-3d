#pragma once

// T17 provider COM host support: the Shell activation sequence, the deterministic
// placeholder scene, the real pipeline composition and the process-resource
// sampling the leak loop uses.
//
// The host loads the built Preview3DThumbnailProvider.dll the way the Shell does
// -- the two COM entry points are PRIVATE, so they are resolved with
// GetProcAddress and never linked (ADR-0010) -- and drives
//   DllGetClassObject -> IClassFactory::CreateInstance -> IInitializeWithStream
//   -> IThumbnailProvider::GetThumbnail
// for a family CLSID.
//
// Separately, the host compiles the *real* provider orchestration
// (ThumbnailPipeline.cpp, GeometrySampler.cpp, CpuRasterizer.cpp) and renders a
// deterministic placeholder scene through it with the T14 sampler and T15
// rasterizer that the DLL links. That proves the golden comparator end to end
// before the first real family adapter exists; a family task swaps the
// placeholder for its routed adapter (see FixtureRegistry.h).

#include "AllocationLedger.h"
#include "CpuRasterizerImpl.h"
#include "Deadline.h"
#include "DeterministicGeometrySampler.h"
#include "FamilyAdapterRegistry.h"
#include "FamilyRouting.h"
#include "FixtureRegistry.h"
#include "ProviderLimits.h"
#include "ThumbnailPipeline.h"

#include "model_core/MaterialPayload.h"

#include <windows.h>
#include <objbase.h>
#include <objidl.h>
#include <propsys.h>
#include <psapi.h>
#include <thumbcache.h>
#include <tlhelp32.h>
#include <unknwn.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace preview3d::test {

using preview3d::provider::Family;
using preview3d::provider::ProviderOutcome;
using preview3d::provider::RasterImage;

using GetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
using CanUnloadNowFn = HRESULT(WINAPI*)();

// Loads the built provider DLL and resolves its two PRIVATE exports.
struct ProviderModule {
    HMODULE handle = nullptr;
    GetClassObjectFn getClassObject = nullptr;
    CanUnloadNowFn canUnloadNow = nullptr;

    ProviderModule()
    {
        handle = ::LoadLibraryExW(PREVIEW3D_PROVIDER_DLL, nullptr, 0);
        if (handle != nullptr) {
            getClassObject = reinterpret_cast<GetClassObjectFn>(
                ::GetProcAddress(handle, "DllGetClassObject"));
            canUnloadNow =
                reinterpret_cast<CanUnloadNowFn>(::GetProcAddress(handle, "DllCanUnloadNow"));
        }
    }

    ProviderModule(const ProviderModule&) = delete;
    ProviderModule& operator=(const ProviderModule&) = delete;

    ~ProviderModule()
    {
        if (handle != nullptr) {
            ::FreeLibrary(handle);
        }
    }

    bool Ready() const noexcept
    {
        return handle != nullptr && getClassObject != nullptr && canUnloadNow != nullptr;
    }
};

// Stack-owned IStream double: `Release` never deletes, so the caller owns it for
// its whole scope and the provider only releases the reference it added.
class MemoryStream final : public IStream {
public:
    explicit MemoryStream(std::vector<std::byte> data) : data_(std::move(data)) {}

    MemoryStream(const MemoryStream&) = delete;
    MemoryStream& operator=(const MemoryStream&) = delete;

    HRESULT WINAPI QueryInterface(REFIID riid, void** ppvObject) noexcept override
    {
        if (ppvObject == nullptr) {
            return E_POINTER;
        }
        *ppvObject = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IStream))) {
            *ppvObject = static_cast<IStream*>(this);
            ++ref_;
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG WINAPI AddRef() noexcept override { return static_cast<ULONG>(++ref_); }
    ULONG WINAPI Release() noexcept override { return static_cast<ULONG>(--ref_); }

    HRESULT WINAPI Read(void* pv, ULONG cb, ULONG* pcbRead) noexcept override
    {
        if (pv == nullptr || pcbRead == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        ULONG count = 0;
        if (position_ < data_.size()) {
            const std::uint64_t available = static_cast<std::uint64_t>(data_.size() - position_);
            count = static_cast<ULONG>((std::min)(available, static_cast<std::uint64_t>(cb)));
            if (count != 0) {
                std::memcpy(pv, data_.data() + position_, count);
                position_ += count;
            }
        }
        *pcbRead = count;
        return S_OK;
    }

    HRESULT WINAPI Write(const void*, ULONG, ULONG*) noexcept override
    {
        return STG_E_ACCESSDENIED;
    }

    HRESULT WINAPI Seek(LARGE_INTEGER move, DWORD origin,
                        ULARGE_INTEGER* newPosition) noexcept override
    {
        std::int64_t base = 0;
        switch (origin) {
            case STREAM_SEEK_SET: base = 0; break;
            case STREAM_SEEK_CUR: base = static_cast<std::int64_t>(position_); break;
            case STREAM_SEEK_END: base = static_cast<std::int64_t>(data_.size()); break;
            default: return STG_E_INVALIDFUNCTION;
        }
        const std::int64_t next = base + move.QuadPart;
        if (next < 0) {
            return STG_E_INVALIDFUNCTION;
        }
        position_ = static_cast<std::uint64_t>(next);
        if (newPosition != nullptr) {
            newPosition->QuadPart = position_;
        }
        return S_OK;
    }

    HRESULT WINAPI SetSize(ULARGE_INTEGER) noexcept override { return E_NOTIMPL; }
    HRESULT WINAPI CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) noexcept override
    {
        return E_NOTIMPL;
    }
    HRESULT WINAPI Commit(DWORD) noexcept override { return S_OK; }
    HRESULT WINAPI Revert() noexcept override { return E_NOTIMPL; }
    HRESULT WINAPI LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) noexcept override
    {
        return E_NOTIMPL;
    }
    HRESULT WINAPI UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) noexcept override
    {
        return E_NOTIMPL;
    }
    HRESULT WINAPI Stat(STATSTG* pstatstg, DWORD) noexcept override
    {
        if (pstatstg == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        std::memset(pstatstg, 0, sizeof(*pstatstg));
        pstatstg->type = STGTY_STREAM;
        pstatstg->cbSize.QuadPart = static_cast<ULONGLONG>(data_.size());
        return S_OK;
    }
    HRESULT WINAPI Clone(IStream**) noexcept override { return E_NOTIMPL; }

private:
    std::vector<std::byte> data_;
    std::uint64_t position_ = 0;
    LONG ref_ = 1;
};

// Minimal BoundedSource over committed fixture bytes. A family fixture routes a
// real adapter, so this double must serve the source through both the
// contiguous view and bounded range reads exactly as the T12 source would.
class MemorySource final : public preview3d::provider::BoundedSource {
public:
    explicit MemorySource(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}

    std::uint64_t Size() const noexcept override { return bytes_.size(); }
    bool Seekable() const noexcept override { return true; }

    bool ReadAt(std::uint64_t offset, std::span<std::byte> dest) override
    {
        if (offset > bytes_.size() || dest.size() > bytes_.size() - offset) {
            return false;
        }
        if (!dest.empty()) {
            std::memcpy(dest.data(), bytes_.data() + offset, dest.size());
        }
        return true;
    }

    std::span<const std::byte> ContiguousView() override
    {
        return {bytes_.data(), bytes_.size()};
    }

private:
    std::vector<std::byte> bytes_;
};

inline GUID GuidFromText(const char* text)
{
    const std::wstring wide(text, text + std::char_traits<char>::length(text));
    GUID guid{};
    if (::CLSIDFromString(wide.c_str(), &guid) != S_OK) {
        guid = GUID{};
    }
    return guid;
}

inline GUID FamilyClsid(preview3d::provider::Family family)
{
    for (const preview3d::provider::FamilyRoute& route : preview3d::provider::FamilyRoutes()) {
        if (route.family == family) {
            return GuidFromText(route.clsid);
        }
    }
    return GUID{};
}

// ---------------------------------------------------------------------------
// Deterministic placeholder scene and adapter
// ---------------------------------------------------------------------------

namespace detail {

struct Gradient {
    float r;
    float g;
    float b;
};

inline Gradient WarmCool(double t)
{
    const Gradient cool{0.06f, 0.16f, 0.52f};
    const Gradient warm{0.86f, 0.42f, 0.05f};
    const float f = static_cast<float>((std::clamp)(t, 0.0, 1.0));
    return {cool.r + (warm.r - cool.r) * f, cool.g + (warm.g - cool.g) * f,
            cool.b + (warm.b - cool.b) * f};
}

inline void SetVertex(preview3d::provider::VertexSample& vertex, double x, double y, double z,
                      const Gradient& color)
{
    vertex.position[0] = static_cast<float>(x);
    vertex.position[1] = static_cast<float>(y);
    vertex.position[2] = static_cast<float>(z);
    const double len = std::sqrt(x * x + y * y + z * z);
    if (len > 1e-12) {
        vertex.normal[0] = static_cast<float>(x / len);
        vertex.normal[1] = static_cast<float>(y / len);
        vertex.normal[2] = static_cast<float>(z / len);
    }
    vertex.color[0] = color.r;
    vertex.color[1] = color.g;
    vertex.color[2] = color.b;
    vertex.color[3] = 1.0f;
}

// The same fixed UV sphere the T15 rasterizer golden uses: a deterministic,
// well-lit scene that exercises the full sampler + rasterizer path.
inline const std::vector<preview3d::provider::TriangleSample>& PlaceholderTriangles()
{
    static const std::vector<preview3d::provider::TriangleSample> kTriangles = [] {
        constexpr int kStacks = 16;
        constexpr int kSlices = 24;
        constexpr double kRadius = 1.6;
        constexpr double kPi = 3.14159265358979323846;

        auto point = [](int stack, int slice) {
            const double phi = kPi * static_cast<double>(stack) / kStacks;
            const double theta = 2.0 * kPi * static_cast<double>(slice) / kSlices;
            const double x = std::sin(phi) * std::cos(theta) * kRadius;
            const double y = std::cos(phi) * kRadius;
            const double z = std::sin(phi) * std::sin(theta) * kRadius;
            preview3d::provider::VertexSample vertex{};
            SetVertex(vertex, x, y, z, WarmCool((y / kRadius + 1.0) * 0.5));
            return vertex;
        };

        std::vector<preview3d::provider::TriangleSample> triangles;
        triangles.reserve(static_cast<std::size_t>(kStacks) * kSlices * 2);
        for (int stack = 0; stack < kStacks; ++stack) {
            for (int slice = 0; slice < kSlices; ++slice) {
                const int nextSlice = (slice + 1) % kSlices;
                const auto a = point(stack, slice);
                const auto b = point(stack + 1, slice);
                const auto c = point(stack + 1, nextSlice);
                const auto d = point(stack, nextSlice);
                preview3d::provider::TriangleSample t0{};
                t0.vertices[0] = a;
                t0.vertices[1] = b;
                t0.vertices[2] = c;
                t0.materialIndex = 1;
                triangles.push_back(t0);
                preview3d::provider::TriangleSample t1{};
                t1.vertices[0] = a;
                t1.vertices[1] = c;
                t1.vertices[2] = d;
                t1.materialIndex = 1;
                triangles.push_back(t1);
            }
        }
        return triangles;
    }();
    return kTriangles;
}

} // namespace detail

// Emits the fixed placeholder scene regardless of the stream contents. It is
// the stand-in that proves the harness before T21 links a real adapter; it is
// not product code and is never compiled into the DLL.
class PlaceholderAdapter final : public preview3d::provider::IFamilyAdapter {
public:
    preview3d::provider::ErrorCode Initialize(
        const preview3d::provider::AdapterInput&) noexcept override
    {
        return preview3d::provider::ErrorCode::None;
    }

    preview3d::provider::ErrorCode Parse() noexcept override
    {
        return preview3d::provider::ErrorCode::None;
    }

    preview3d::provider::ErrorCode EnumerateMaterials(
        preview3d::provider::IMaterialSink& sink) noexcept override
    {
        sink.OnMaterial(1, preview3d::provider::NeutralMaterial());
        return preview3d::provider::ErrorCode::None;
    }

    preview3d::provider::ErrorCode EnumerateGeometry(
        preview3d::provider::IGeometrySink& sink) noexcept override
    {
        for (const preview3d::provider::TriangleSample& triangle : detail::PlaceholderTriangles()) {
            if (!sink.OnTriangle(triangle)) {
                break;
            }
        }
        return preview3d::provider::ErrorCode::None;
    }

    void Reset() noexcept override {}
};

// The host's IThumbnailDependencies: the placeholder for Family::Unknown and the
// real, CLSID-routed registry for every family a task has linked in this build,
// with the shipped T14 sampler and T15 rasterizer.
class HostDependencies final : public preview3d::provider::IThumbnailDependencies {
public:
    std::unique_ptr<preview3d::provider::IFamilyAdapter> CreateAdapter(Family family) noexcept override
    {
        if (family == Family::Unknown) {
            return std::make_unique<PlaceholderAdapter>();
        }
        return preview3d::provider::CreateFamilyAdapter(family);
    }

    std::unique_ptr<preview3d::provider::IGeometrySampler> CreateSampler() noexcept override
    {
        try {
            return std::make_unique<preview3d::provider::DeterministicGeometrySampler>();
        } catch (...) {
            return nullptr;
        }
    }

    preview3d::provider::ErrorCode Render(
        const preview3d::provider::RasterRequest& request,
        RasterImage& out) noexcept override
    {
        return preview3d::provider::RenderCpuTileRaster(request, out);
    }
};

// Runs one fixture through the real pipeline. Returns the tabulated outcome and
// fills `out` only on Success.
inline ProviderOutcome RunFixture(const GoldenFixture& fixture, RasterImage& out)
{
    MemorySource source(std::vector<std::byte>(fixture.source));
    preview3d::provider::Deadline deadline;
    preview3d::provider::AllocationLedger ledger;

    preview3d::provider::ThumbnailRequest request{};
    request.family = fixture.family;
    request.source = &source;
    request.limits = &preview3d::provider::ProviderLimits::Default();
    request.deadline = &deadline;
    request.ledger = &ledger;
    request.cx = fixture.cx;

    HostDependencies dependencies;
    return preview3d::provider::RunThumbnailPipeline(request, dependencies, out);
}

// ---------------------------------------------------------------------------
// Process resource sampling for the load/unload leak loop
// ---------------------------------------------------------------------------

struct ProcessResources {
    DWORD gdiObjects = 0;
    DWORD userObjects = 0;
    ULONGLONG privateBytes = 0;
    DWORD threads = 0;
};

inline DWORD CountProcessThreads()
{
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD pid = ::GetCurrentProcessId();
    DWORD count = 0;
    if (::Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == pid) {
                ++count;
            }
        } while (::Thread32Next(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return count;
}

inline ProcessResources SampleProcessResources()
{
    ProcessResources resources;
    resources.gdiObjects = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
    resources.userObjects = ::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS);
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters))) {
        resources.privateBytes = counters.PagefileUsage;
    }
    resources.threads = CountProcessThreads();
    return resources;
}

// ---------------------------------------------------------------------------
// One activate/use/unload cycle through the real COM path
// ---------------------------------------------------------------------------

// Performs the Shell sequence once against a freshly loaded provider module and
// returns the GetThumbnail result. Every COM object is released; the module is
// unloaded at the end of the caller's scope. `bitmap` is owned by the caller.
inline HRESULT ActivateAndRender(ProviderModule& module, REFCLSID clsid, IStream& stream,
                                 std::uint32_t cx, HBITMAP& bitmap, WTS_ALPHATYPE& alpha)
{
    bitmap = nullptr;
    alpha = WTSAT_UNKNOWN;

    void* raw = nullptr;
    const HRESULT classHr =
        module.getClassObject(clsid, __uuidof(IClassFactory), &raw);
    if (FAILED(classHr)) {
        return classHr;
    }
    auto* factory = static_cast<IClassFactory*>(raw);

    IThumbnailProvider* provider = nullptr;
    HRESULT hr = factory->CreateInstance(nullptr, __uuidof(IThumbnailProvider),
                                         reinterpret_cast<void**>(&provider));
    factory->Release();
    if (FAILED(hr) || provider == nullptr) {
        return FAILED(hr) ? hr : E_UNEXPECTED;
    }

    IInitializeWithStream* init = nullptr;
    hr = provider->QueryInterface(__uuidof(IInitializeWithStream),
                                  reinterpret_cast<void**>(&init));
    if (SUCCEEDED(hr) && init != nullptr) {
        hr = init->Initialize(&stream, STGM_READ);
        if (SUCCEEDED(hr)) {
            hr = provider->GetThumbnail(cx, &bitmap, &alpha);
        }
        init->Release();
    }
    provider->Release();
    return hr;
}

} // namespace preview3d::test
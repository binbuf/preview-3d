// T11 provider COM core implementation (see ComCore.h and ADR-0013).

#include "pch.h"

#include "ComCore.h"

// T12: the provider object also implements IInitializeWithStream over the
// bounded stream source. propsys.h declares the interface; only __uuidof is
// used, so no propsys/ole32 import is added.
//
// T13 adds IThumbnailProvider to the same object and routes GetThumbnail
// through ThumbnailPipeline (ADR-0015); thumbcache.h declares the interface and
// WTS_ALPHATYPE, and gdi32 provides CreateDIBSection. Only __uuidof is used, so
// no thumbcache import is added.
#include "AllocationLedger.h"
#include "Containment.h"
#include "Deadline.h"
#include "Diagnostics.h"
#include "ProviderErrors.h"
#include "ProviderLimits.h"
#include "RasterBitmap.h"
#include "StreamSource.h"
#include "ThumbnailPipeline.h"

#include <objbase.h>
#include <propsys.h>
#include <thumbcache.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>

namespace preview3d::provider {
namespace {

// The module handle. The lifetime counters and ActiveCallGuard live in
// ModuleLifetime.{h,cpp} (T16); they are the one process-global mutable state
// and are pure reference bookkeeping, never a cache and never model data.
HMODULE g_module = nullptr;
constexpr char kHexDigits[] = "0123456789ABCDEF";

// Formats a GUID as the canonical upper-case, brace-wrapped CLSID string the
// routing table stores ({XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}). Done locally
// with no StringFromGUID2/ole32 import so the provider stays a two-import DLL.
void FormatClsid(REFGUID clsid, char out[40]) noexcept
{
    const auto put = [&out](std::size_t& at, std::uint32_t value, int digits) noexcept {
        for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) {
            out[at++] = kHexDigits[(value >> shift) & 0x0Fu];
        }
    };

    std::size_t at = 0;
    out[at++] = '{';
    put(at, clsid.Data1, 8);
    out[at++] = '-';
    put(at, clsid.Data2, 4);
    out[at++] = '-';
    put(at, clsid.Data3, 4);
    out[at++] = '-';
    put(at, clsid.Data4[0], 2);
    put(at, clsid.Data4[1], 2);
    out[at++] = '-';
    for (int i = 2; i < 8; ++i) {
        put(at, clsid.Data4[i], 2);
    }
    out[at++] = '}';
    out[at] = '\0';
}

// Context for the contained pipeline call. Kept as a plain struct so the
// contained call is a fixed function pointer rather than a capturing lambda.
struct PipelineCall {
    const ThumbnailRequest* request = nullptr;
    IThumbnailDependencies* dependencies = nullptr;
    RasterImage* image = nullptr;
};

ProviderOutcome InvokePipeline(void* context) noexcept
{
    auto* call = static_cast<PipelineCall*>(context);
    return RunThumbnailPipeline(*call->request, *call->dependencies, *call->image);
}

// Context for the contained DIB/HBITMAP boundary call.
struct BitmapCall {
    const RasterImage* image = nullptr;
    HBITMAP* bitmap = nullptr;
};

ProviderOutcome InvokeBitmap(void* context) noexcept
{
    auto* call = static_cast<BitmapCall*>(context);
    return CreatePremultipliedDib(*call->image, *call->bitmap);
}

// The provider object. T11 owns identity/QI/refcount; T12 adds
// IInitializeWithStream over BoundedStreamSource and T13 adds
// IThumbnailProvider to this same class, and T16 wraps GetThumbnail in
// ActiveCallGuard. Aggregation is not supported, so CreateInstance rejects a
// non-null pUnkOuter before an instance exists.
class ProviderObject final : public IInitializeWithStream, public IThumbnailProvider {
public:
    explicit ProviderObject(Family family) noexcept : family_(family)
    {
        ModuleLifetime::AddObject();
    }

    ~ProviderObject() noexcept { ModuleLifetime::ReleaseObject(); }

    Family FamilyValue() const noexcept { return family_; }

    // The bounded source adopted by Initialize, for T13's GetThumbnail caller.
    // Null until a successful Initialize.
    BoundedStreamSource* StreamSource() const noexcept { return source_.get(); }

    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) noexcept override
    {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown))) {
            // Two IUnknown bases are inherited (IInitializeWithStream and
            // IThumbnailProvider), so the upcast is disambiguated through one.
            *ppv = static_cast<IUnknown*>(static_cast<IInitializeWithStream*>(this));
            AddRef();
            return S_OK;
        }
        if (IsEqualIID(riid, __uuidof(IInitializeWithStream))) {
            *ppv = static_cast<IInitializeWithStream*>(this);
            AddRef();
            return S_OK;
        }
        if (IsEqualIID(riid, __uuidof(IThumbnailProvider))) {
            *ppv = static_cast<IThumbnailProvider*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG WINAPI AddRef() noexcept override
    {
        return static_cast<ULONG>(ref_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG WINAPI Release() noexcept override
    {
        const ULONG remaining = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    // IInitializeWithStream (T12). Accepts exactly one non-null stream, takes an
    // independent reference through the bounded source and rejects a second
    // initialization. A null stream is E_POINTER; an already-adopted stream is
    // E_UNEXPECTED; preflight caps and the ledger map through the T06 table.
    HRESULT WINAPI Initialize(IStream* pstream, DWORD grfMode) noexcept override
    {
        (void)grfMode;
        if (pstream == nullptr) {
            return E_POINTER;
        }
        if (source_ != nullptr) {
            return E_UNEXPECTED;
        }

        // T16: this bounded COM call keeps the module loaded for its duration.
        ActiveCallGuard guard;

        ProviderOutcome outcome = ProviderOutcome::Success;
        auto source = BoundedStreamSource::Create(
            pstream, Deadline{}, AllocationLedger::ProcessWide(), outcome);
        if (!source) {
            return HresultFor(outcome);
        }
        source_ = std::move(source);
        return S_OK;
    }

    // IThumbnailProvider (T13). `cx` is the caller's maximum physical-pixel
    // hint; only the degenerate cx == 0 is rejected (E_INVALIDARG, ADR-0015)
    // and the actual resolution is clamped independently by the rasterizer.
    // The family was fixed by the object's CLSID, so no content is sniffed. On
    // every failure both out-params stay null/unknown; a success bitmap is
    // never fabricated (design/05, "HRESULT mapping").
    HRESULT WINAPI GetThumbnail(UINT cx, HBITMAP* phbmp,
                                WTS_ALPHATYPE* pdwAlpha) noexcept override
    {
        if (phbmp == nullptr || pdwAlpha == nullptr) {
            return E_POINTER;
        }
        *phbmp = nullptr;
        *pdwAlpha = WTSAT_UNKNOWN;

        if (source_ == nullptr) {
            return HresultFor(ProviderOutcome::InvalidCallSequence);
        }
        if (cx == 0) {
            return HresultFor(ProviderOutcome::BadArgument);
        }

        // T16: this bounded COM call keeps the module loaded for its duration,
        // even if the caller releases the object concurrently.
        ActiveCallGuard guard;

        // Restart the per-object read deadline for this call (ADR-0014).
        source_->MutableDeadline().Restart();
        Deadline& deadline = source_->MutableDeadline();

        ThumbnailRequest request{};
        request.family = family_;
        request.source = source_.get();
        request.limits = &ProviderLimits::Default();
        request.deadline = &deadline;
        request.ledger = &AllocationLedger::ProcessWide();
        request.cx = static_cast<std::uint32_t>(cx);

        // Every stage runs on the calling thread (design/05, "Threading and
        // unload"); no pool or worker is created. The pipeline and the DIB
        // boundary are the last-resort containment points: a C++ exception or a
        // structured exception that escapes a third-party parser is translated
        // to the tabulated HRESULT instead of taking down the surrogate. An
        // uninterruptible call that returned after the cooperative stop point is
        // rejected after the fact, with its real elapsed time recorded.
        RasterImage image;
        PipelineCall pipelineCall{&request, &DefaultThumbnailDependencies(), &image};
        const ContainmentResult containedPipeline = RunContained(
            &InvokePipeline, &pipelineCall, &deadline, DiagnosticStage::Render);
        if (containedPipeline.outcome != ProviderOutcome::Success) {
            return HresultFor(containedPipeline.outcome);
        }

        HBITMAP bitmap = nullptr;
        BitmapCall bitmapCall{&image, &bitmap};
        const ContainmentResult containedBitmap = RunContained(
            &InvokeBitmap, &bitmapCall, &deadline, DiagnosticStage::Bitmap);
        if (containedBitmap.outcome != ProviderOutcome::Success) {
            if (bitmap != nullptr) {
                ::DeleteObject(bitmap);
            }
            return HresultFor(containedBitmap.outcome);
        }

        *phbmp = bitmap;
        *pdwAlpha = WTSAT_ARGB;
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
    Family family_;
    std::unique_ptr<BoundedStreamSource> source_;
};

// One class-factory instance per routed family CLSID. It holds a module object
// reference for its lifetime and forwards LockServer() to the module lock count.
class FamilyClassFactory final : public IClassFactory {
public:
    explicit FamilyClassFactory(Family family) noexcept : family_(family)
    {
        ModuleLifetime::AddObject();
    }

    ~FamilyClassFactory() noexcept { ModuleLifetime::ReleaseObject(); }

    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) noexcept override
    {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(IClassFactory))) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG WINAPI AddRef() noexcept override
    {
        return static_cast<ULONG>(ref_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG WINAPI Release() noexcept override
    {
        const ULONG remaining = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT WINAPI CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) noexcept override
    {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;

        // The provider object is not aggregatable: a containing object is
        // rejected before any instance is constructed.
        if (pUnkOuter != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }

        ActiveCallGuard guard;
        auto* object = new (std::nothrow) ProviderObject(family_);
        if (object == nullptr) {
            return E_OUTOFMEMORY;
        }

        const HRESULT hr = object->QueryInterface(riid, ppv);
        object->Release();
        return hr;
    }

    HRESULT WINAPI LockServer(BOOL fLock) noexcept override
    {
        if (fLock != FALSE) {
            ModuleLifetime::AddLock();
        } else {
            ModuleLifetime::ReleaseLock();
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
    Family family_;
};

} // namespace

void RecordModuleHandle(HMODULE module) noexcept { g_module = module; }

HMODULE ModuleHandle() noexcept { return g_module; }

HRESULT GetClassObject(REFCLSID clsid, REFIID riid, void** ppv) noexcept
{
    if (ppv == nullptr) {
        return E_POINTER;
    }
    *ppv = nullptr;

    char clsidText[40] = {};
    FormatClsid(clsid, clsidText);
    const FamilyRoute* route = RouteForClsid(std::string_view(clsidText));
    if (route == nullptr) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    auto* factory = new (std::nothrow) FamilyClassFactory(route->family);
    if (factory == nullptr) {
        return E_OUTOFMEMORY;
    }

    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

} // namespace preview3d::provider
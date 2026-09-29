// T11 provider COM core implementation (see ComCore.h and ADR-0013).

#include "pch.h"

#include "ComCore.h"

// T12: the provider object also implements IInitializeWithStream over the
// bounded stream source. propsys.h declares the interface; only __uuidof is
// used, so no propsys/ole32 import is added.
#include "AllocationLedger.h"
#include "Deadline.h"
#include "ProviderErrors.h"
#include "StreamSource.h"

#include <objbase.h>
#include <propsys.h>

#include <memory>
#include <new>

namespace preview3d::provider {
namespace {

// The one process-global mutable state: pure lifetime reference counts, never a
// cache and never model data.
HMODULE g_module = nullptr;
constexpr char kHexDigits[] = "0123456789ABCDEF";
std::atomic<std::uint32_t> g_liveObjects{0};
std::atomic<std::uint32_t> g_locks{0};
std::atomic<std::uint32_t> g_activeCalls{0};

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

// The provider object. T11 owns identity/QI/refcount; T12 adds
// IInitializeWithStream over BoundedStreamSource and T13 adds
// IThumbnailProvider to this same class, and T16 wraps GetThumbnail in
// ActiveCallGuard. Aggregation is not supported, so CreateInstance rejects a
// non-null pUnkOuter before an instance exists.
class ProviderObject final : public IInitializeWithStream {
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
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        if (IsEqualIID(riid, __uuidof(IInitializeWithStream))) {
            *ppv = static_cast<IInitializeWithStream*>(this);
            AddRef();
            return S_OK;
        }
        // IThumbnailProvider (T13) joins here.
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

        ProviderOutcome outcome = ProviderOutcome::Success;
        auto source = BoundedStreamSource::Create(
            pstream, Deadline{}, AllocationLedger::ProcessWide(), outcome);
        if (!source) {
            return HresultFor(outcome);
        }
        source_ = std::move(source);
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

namespace ModuleLifetime {

void AddObject() noexcept { g_liveObjects.fetch_add(1, std::memory_order_relaxed); }
void ReleaseObject() noexcept { g_liveObjects.fetch_sub(1, std::memory_order_acq_rel); }
void AddLock() noexcept { g_locks.fetch_add(1, std::memory_order_relaxed); }
void ReleaseLock() noexcept { g_locks.fetch_sub(1, std::memory_order_acq_rel); }
void AddActiveCall() noexcept { g_activeCalls.fetch_add(1, std::memory_order_relaxed); }
void ReleaseActiveCall() noexcept { g_activeCalls.fetch_sub(1, std::memory_order_acq_rel); }

bool CanUnloadNow() noexcept
{
    return g_liveObjects.load(std::memory_order_acquire) == 0 &&
           g_locks.load(std::memory_order_acquire) == 0 &&
           g_activeCalls.load(std::memory_order_acquire) == 0;
}

} // namespace ModuleLifetime

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
#pragma once

// T11 provider COM core: the CLSID-routed class factory, the provider object
// shell and the module/object/lock/active-call lifetime bookkeeping.
//
// This header is the only place the DLL's COM entry points (ProviderExports.cpp)
// and DllMain reach for their implementation. It owns no policy beyond what
// design/05-thumbnail-provider.md ("COM classes and extension assignment",
// "Threading and unload") and ADR-0013 fix:
//
//  - the family is selected from the object's CLSID via FamilyRouting.h and
//    nothing else -- no content sniffing, no path recovery;
//  - factory objects, provider objects, IClassFactory::LockServer() locks and
//    in-flight calls are counted explicitly, and DllCanUnloadNow returns S_OK
//    only when all of them are zero;
//  - DllMain is side-effect-free: it records the module handle and disables the
//    thread notifications this server does not use.
//
// The counters are the one process-global mutable state the DLL owns, and they
// are pure reference bookkeeping -- not a cache and not model data. The provider
// object implements only IUnknown here; T12 adds IInitializeWithStream and T13
// adds IThumbnailProvider to the same class.

#include "FamilyRouting.h"

#include <windows.h>
#include <unknwn.h>

#include <atomic>
#include <cstdint>

namespace preview3d::provider {

// -- Module identity ---------------------------------------------------------

// Called once from DllMain(DLL_PROCESS_ATTACH). DllMain does nothing else:
// no COM, no registration, no library load, no thread creation (design/05).
void RecordModuleHandle(HMODULE module) noexcept;
HMODULE ModuleHandle() noexcept;

// -- Lifetime bookkeeping ----------------------------------------------------

// Explicit module/object/lock/active-call reference counts (design/05,
// "Threading and unload"). A live COM object holds one object reference for its
// lifetime; IClassFactory::LockServer(TRUE) adds a lock; a bounded COM call is
// wrapped in ActiveCallGuard so it keeps the module alive even if the caller
// releases every external reference concurrently.
//
// CanUnloadNow() is the single predicate behind DllCanUnloadNow. It returns true
// only when the object, lock and active-call counts are all zero.
namespace ModuleLifetime {

void AddObject() noexcept;
void ReleaseObject() noexcept;
void AddLock() noexcept;
void ReleaseLock() noexcept;
void AddActiveCall() noexcept;
void ReleaseActiveCall() noexcept;

bool CanUnloadNow() noexcept;

} // namespace ModuleLifetime

// RAII marker for an in-flight bounded call.
class ActiveCallGuard {
public:
    ActiveCallGuard() noexcept { ModuleLifetime::AddActiveCall(); }
    ActiveCallGuard(const ActiveCallGuard&) = delete;
    ActiveCallGuard& operator=(const ActiveCallGuard&) = delete;
    ActiveCallGuard(ActiveCallGuard&&) = delete;
    ActiveCallGuard& operator=(ActiveCallGuard&&) = delete;
    ~ActiveCallGuard() noexcept { ModuleLifetime::ReleaseActiveCall(); }
};

// -- COM entry point ---------------------------------------------------------

// The IClassFactory implementation behind DllGetClassObject. Unknown CLSIDs
// return CLASS_E_CLASSNOTAVAILABLE; `riid` must be IID_IUnknown or
// IID_IClassFactory; a null `ppv` returns E_POINTER. The factory it returns
// selects the adapter family from the frozen routing table.
HRESULT GetClassObject(REFCLSID clsid, REFIID riid, void** ppv) noexcept;

} // namespace preview3d::provider
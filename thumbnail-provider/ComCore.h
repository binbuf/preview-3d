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
// object implements IUnknown and IInitializeWithStream (T12); T13 adds
// IThumbnailProvider to the same class.

#include "FamilyRouting.h"
#include "ModuleLifetime.h"

#include <windows.h>
#include <unknwn.h>

#include <cstdint>

namespace preview3d::provider {

// -- Module identity ---------------------------------------------------------

// Called once from DllMain(DLL_PROCESS_ATTACH). DllMain does nothing else:
// no COM, no registration, no library load, no thread creation (design/05).
void RecordModuleHandle(HMODULE module) noexcept;
HMODULE ModuleHandle() noexcept;

// -- Lifetime bookkeeping ----------------------------------------------------

// The explicit module/object/lock/active-call reference counts and the RAII
// `ActiveCallGuard` live in ModuleLifetime.h (T16 extracted them from this
// header so Tests.Unit.exe can prove the active-call unload gate directly).
// design/05 ("Threading and unload") and ADR-0013 fix their contract.

// -- COM entry point ---------------------------------------------------------

// The IClassFactory implementation behind DllGetClassObject. Unknown CLSIDs
// return CLASS_E_CLASSNOTAVAILABLE; `riid` must be IID_IUnknown or
// IID_IClassFactory; a null `ppv` returns E_POINTER. The factory it returns
// selects the adapter family from the frozen routing table.
HRESULT GetClassObject(REFCLSID clsid, REFIID riid, void** ppv) noexcept;

} // namespace preview3d::provider
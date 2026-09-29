// T05 scaffolding for the provider's COM in-proc server entry points.
//
// The linker's module-definition file (Preview3DThumbnailProvider.def) exports
// exactly these two symbols and nothing else, so the DLL links and the
// export-surface / dependency-closure tests in Tests.Unit.exe can run before the
// real implementation exists. T11 replaces these bodies with the class factory
// and module/object/lock reference counts; the signatures and the export surface
// are frozen here.
//
// Implementing full COM behavior is explicitly out of scope for T05 (→ T11).

#include "pch.h"

#include <objbase.h>

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID, REFIID, void** ppv)
{
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT WINAPI DllCanUnloadNow()
{
    return S_OK;
}
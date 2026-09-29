// T11 provider COM in-proc server entry points.
//
// The linker's module-definition file (Preview3DThumbnailProvider.def) exports
// exactly these two symbols and nothing else, both PRIVATE (ADR-0010). The class
// factory and lifetime bookkeeping live in ComCore.h/.cpp; this translation unit
// is only the stable entry-point surface the Shell (and the T17 host) resolves
// with GetProcAddress.
//
// There is deliberately no DllRegisterServer/DllUnregisterServer: registration is
// installer-owned (ADR-0006/0007) and a self-registration export would
// bypass the non-clobber policy in design/05-thumbnail-provider.md.

#include "pch.h"

#include "ComCore.h"

#include <objbase.h>

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    return preview3d::provider::GetClassObject(clsid, riid, ppv);
}

extern "C" HRESULT WINAPI DllCanUnloadNow()
{
    return preview3d::provider::ModuleLifetime::CanUnloadNow() ? S_OK : S_FALSE;
}
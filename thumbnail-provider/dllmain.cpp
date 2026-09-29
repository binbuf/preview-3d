// dllmain.cpp : Defines the entry point for the DLL application.
//
// DllMain is side-effect-free (design/05-thumbnail-provider.md): it records the
// module handle for the COM core and disables the thread notifications this
// in-proc server does not use. It does not call CoCreateInstance, load a
// library, register anything, or create a thread.

#include "pch.h"

#include "ComCore.h"

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    UNREFERENCED_PARAMETER(lpReserved);
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        preview3d::provider::RecordModuleHandle(hModule);
        ::DisableThreadLibraryCalls(hModule);
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
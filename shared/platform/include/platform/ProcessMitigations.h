#pragma once

#include <windows.h>

namespace platform {

// Applies the process-mitigation set shared by every Preview 3D executable:
// heap termination on corruption, extension-point disable, and (optionally)
// ACG (ProhibitDynamicCode). ACG is safe for the pinned parsers but is withheld
// from the viewer until a GPU run validates the D3D runtime.
//
// CFG is not set here: the image is already compiled and linked with /guard:cf
// (and the broker forces it always-on at creation for the import children), and
// calling SetProcessMitigationPolicy(ProcessControlFlowGuardPolicy) on an
// already-CFG process is denied. Signature enforcement (MicrosoftSignedOnly) is
// deliberately absent until the payload is Authenticode-signed (SEC-14): the
// images and app-local DLLs are unsigned today and that policy would make them
// unloadable.
//
// Returns false when a required policy cannot be applied, so callers fail
// closed rather than running with a silently missing mitigation.
inline bool ApplyProcessMitigations(bool prohibitDynamicCode)
{
    if (!HeapSetInformation(GetProcessHeap(), HeapEnableTerminationOnCorruption, nullptr, 0)) {
        return false;
    }

    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extensionPoints{};
    extensionPoints.DisableExtensionPoints = 1;
    if (!SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &extensionPoints,
                                    sizeof(extensionPoints))) {
        return false;
    }

    if (prohibitDynamicCode) {
        PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicCode{};
        dynamicCode.ProhibitDynamicCode = 1;
        if (!SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &dynamicCode,
                                        sizeof(dynamicCode))) {
            return false;
        }
    }

    return true;
}

} // namespace platform
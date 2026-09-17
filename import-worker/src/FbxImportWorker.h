#pragma once

#include "model_core/ControlProtocol.h"
#include <windows.h>

namespace import_worker {

bool HandleFbxImportFileRequest(HANDLE stdIn, HANDLE stdOut,
                                const model_core::ParseFbxFileRequest& request);
int RunFbxImport();

} // namespace import_worker

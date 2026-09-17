#pragma once

#include "model_core/ControlProtocol.h"
#include <windows.h>

namespace import_worker {

bool HandleObjImportFileRequest(HANDLE stdIn, HANDLE stdOut,
                                const model_core::ParseObjFileRequest& request);
int RunObjImport();

} // namespace import_worker

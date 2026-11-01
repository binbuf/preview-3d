#pragma once

#include "model_core/ControlProtocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace import_worker {

bool HandleAssimpImportFileRequest(HANDLE stdIn, HANDLE stdOut,
                                   const model_core::ParseAssimpFileRequest& request);

int RunAssimpImport();

} // namespace import_worker

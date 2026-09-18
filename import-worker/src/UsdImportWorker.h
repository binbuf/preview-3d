#pragma once

#include "model_core/ControlProtocol.h"

#include <windows.h>

namespace import_worker {

// USD-003 contract route. It proves byte-derived family identity, request
// framing, typed failures, one-shot launch, and pooled dispatch; USD-004
// replaces its metadata-only success payload with the static scene adapter.
bool HandleUsdImportFileRequest(HANDLE stdIn, HANDLE stdOut,
                                const model_core::ParseUsdFileRequest& request);
int RunUsdImport();

} // namespace import_worker

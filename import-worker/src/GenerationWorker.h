#pragma once

#include "model_core/ControlProtocol.h"

#include <windows.h>

namespace import_worker {

// Handles one already-read StartGenerationRequest: maps the inherited
// output section, runs GenerateSyntheticScene, and writes
// ChunksReady/GenerationError to stdOut. Returns true iff ChunksReady was
// sent. Shared by both the one-shot --generate entry point (RunGeneration)
// and the --pool mode's request loop (WorkerRequestDispatch.cpp), so
// pooling adds no duplicated business logic.
bool HandleStartGeneration(HANDLE stdOut, const model_core::StartGenerationRequest& request);

// Owns the --generate mode's control-channel read/dispatch/write sequence:
// reads one StartGenerationRequest from the inherited stdin pipe, maps the
// inherited shared section, runs GenerateSyntheticScene, and reports
// ChunksReady or GenerationError back over the inherited stdout pipe.
// Returns the process exit code (0 on success).
int RunGeneration();

} // namespace import_worker

#pragma once

#include "model_core/ControlProtocol.h"

#include <windows.h>

namespace import_worker {

// Handles one already-read ParsePlyFileRequest: builds a
// model_core::MappedFile from the received raw file handle, maps the
// output section, calls PlyAdapter::ImportPly, writes ChunksReady/
// GenerationError to stdOut. Returns true iff ChunksReady was sent.
// Shared by both the one-shot --parse-ply entry point (RunPlyImport) and
// the --pool mode's request loop (WorkerRequestDispatch.cpp).
bool HandlePlyImportFileRequest(HANDLE stdOut, const model_core::ParsePlyFileRequest& request);

// Owns the --parse-ply mode's control-channel read/dispatch/write
// sequence: reads one ParsePlyFileRequest from the inherited stdin pipe
// and dispatches to HandlePlyImportFileRequest. Returns the process exit
// code (0 on success). Mirrors StlImportWorker::RunStlImport's shape.
int RunPlyImport();

} // namespace import_worker

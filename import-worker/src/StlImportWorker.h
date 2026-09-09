#pragma once

#include "model_core/ControlProtocol.h"

#include <windows.h>

namespace import_worker {

// Handles one already-read ParseStlFileRequest: builds a
// model_core::MappedFile from the received raw file handle, maps the
// output section, calls StlAdapter::ImportStl, writes ChunksReady/
// GenerationError to stdOut. Returns true iff ChunksReady was sent.
// Shared by both the one-shot --parse-stl entry point (RunStlImport) and
// the --pool mode's request loop (WorkerRequestDispatch.cpp).
bool HandleStlImportFileRequest(HANDLE stdOut, const model_core::ParseStlFileRequest& request);

// Owns the --parse-stl mode's control-channel read/dispatch/write
// sequence: reads one ParseStlFileRequest from the inherited stdin pipe
// and dispatches to HandleStlImportFileRequest. Returns the process exit
// code (0 on success). Mirrors GltfImportWorker::RunGltfImport's shape.
int RunStlImport();

} // namespace import_worker

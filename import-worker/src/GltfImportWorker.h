#pragma once

#include "model_core/ControlProtocol.h"

#include <windows.h>

namespace import_worker {

// Handles one already-read ParseGltfRequest -- the pre-made whole-source
// shared/pagefile-section input path (StartGltfImport). Maps source+output
// sections, calls GltfAdapter::ImportGltf, writes ChunksReady/
// GenerationError to stdOut. Returns true iff ChunksReady was sent. Shared
// by both the one-shot --parse-gltf entry point (RunGltfImport) and the
// --pool mode's request loop (WorkerRequestDispatch.cpp).
bool HandleGltfImportRequest(HANDLE stdOut, const model_core::ParseGltfRequest& request);

// Handles one already-read ParseGltfFileRequest -- the real sandboxed-
// pipeline path (StartGltfImportFromFile). Builds a model_core::MappedFile
// from the received raw file handle, maps the output section, calls
// GltfAdapter::ImportGltf, writes ChunksReady/GenerationError to stdOut.
// Returns true iff ChunksReady was sent. Shared the same way as above.
bool HandleGltfImportFileRequest(HANDLE stdOut, const model_core::ParseGltfFileRequest& request);

// Owns the --parse-gltf mode's control-channel read/dispatch/write
// sequence: reads one control message from the inherited stdin pipe (either
// StartGltfImport or StartGltfImportFromFile) and dispatches to
// HandleGltfImportRequest/HandleGltfImportFileRequest. Returns the process
// exit code (0 on success). Mirrors GenerationWorker::RunGeneration's shape
// exactly.
int RunGltfImport();

} // namespace import_worker

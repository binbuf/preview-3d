#pragma once

// The --pool mode's request loop. Every one-shot mode (--generate,
// --parse-gltf) keeps its own strict "read exactly one message of exactly
// this opcode, reject anything else" entry point unchanged (RunGeneration,
// RunGltfImport) -- this file is purely additive, reusing their now-shared
// per-opcode handlers (GenerationWorker::HandleStartGeneration,
// GltfImportWorker::HandleGltfImportRequest/HandleGltfImportFileRequest)
// rather than duplicating any business logic.

#include <windows.h>

namespace import_worker {

enum class DispatchOutcome {
    Continue,      // a request was handled (ChunksReady or GenerationError sent); keep looping
    Shutdown,      // the Shutdown opcode was received; exit cleanly, no reply
    ProtocolError, // EOF, malformed framing, or an unrecognized opcode/payload size
};

// Reads exactly one control message from stdIn and dispatches it to the
// appropriate per-opcode handler, writing its reply to stdOut. A
// GenerationError reply is an ordinary per-request outcome (ProtocolError
// is reserved for a broken/unrecognized message itself, not a failed
// generation) -- callers should keep looping on Continue regardless of
// whether the individual request it represents succeeded.
DispatchOutcome DispatchOneRequest(HANDLE stdIn, HANDLE stdOut);

// --pool mode: loops calling DispatchOneRequest until Shutdown or a
// ProtocolError/EOF. Returns the process exit code.
int RunPoolMode();

} // namespace import_worker

#pragma once

namespace import_worker {

// Owns the --parse-gltf mode's control-channel read/dispatch/write
// sequence: reads one ParseGltfRequest from the inherited stdin pipe, maps
// the inherited input (source GLB bytes) and output sections, calls
// GltfAdapter::ImportGltf, and reports ChunksReady or GenerationError back
// over the inherited stdout pipe. Returns the process exit code (0 on
// success). Mirrors GenerationWorker::RunGeneration's shape exactly.
int RunGltfImport();

} // namespace import_worker

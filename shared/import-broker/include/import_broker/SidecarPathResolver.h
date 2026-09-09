#pragma once

// Constrained sidecar-file resolution, per .docs/design/03-file-formats-and-
// ingestion.md's "Input boundary": a .gltf's referenced .bin/.png/.jpg/
// .jpeg/.webp/.ktx2 sibling must resolve to a file that (a) is a relative,
// local reference -- never absolute, UNC, a device path, or any URI scheme
// -- and (b) canonicalizes (after opening, the same trusted technique
// SourceFileAccess.cpp already uses for the primary file) to a path inside
// the primary file's own directory, so a reparse point cannot smuggle the
// reference outside it. Path authority lives only in this trusted-process
// function; the sandboxed worker never opens a path itself (see
// SidecarRequestServicer.h, which calls this).

#include "model_core/ImportError.h"
#include "platform/Win32Handle.h"

#include <cstdint>
#include <string>

namespace import_broker {

struct SidecarResolution {
    platform::Win32Handle file;    // set only on success
    std::wstring canonicalPath;    // set only on success
    uint64_t fileSizeBytes = 0;    // set only on success
    model_core::ImportErrorCode rejectionCode = model_core::ImportErrorCode::None; // set only on failure
    std::wstring diagnosticMessage; // developer/log-only, never shown raw to the end user
};

// primaryCanonicalPath: the value OpenAndCanonicalizeSourceFile already
// produced for the .gltf itself. relativeReferenceUtf8: the sidecar
// reference as the worker's own parser decoded it (fastgltf::URI::path(),
// UTF-8) -- never re-decoded or re-interpreted as a URI here, just checked
// as a plain relative filesystem path.
SidecarResolution ResolveSidecarPath(const std::wstring& primaryCanonicalPath,
                                      const std::string& relativeReferenceUtf8,
                                      uint64_t maxSidecarFileBytes);

} // namespace import_broker

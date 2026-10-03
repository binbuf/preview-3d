#pragma once

// Constrained sidecar-file resolution, per .docs/design/03-file-formats-and-
// ingestion.md's "Input boundary": a .gltf's referenced .bin/.png/.jpg/
// .jpeg/.webp/.ktx2 sidecar must resolve to a file that (a) is a relative,
// local reference -- never absolute, UNC, a device path, or any URI scheme
// -- and (b) canonicalizes (after opening, the same trusted technique
// SourceFileAccess.cpp already uses for the primary file) to a path inside
// the primary file's own directory tree, so a reparse point cannot smuggle
// the reference outside it. Subdirectories below the primary directory
// (the common "textures/foo.jpg" layout) are inside that tree and resolve;
// siblings and anything above it are rejected. Path authority lives only in this trusted-process
// function; the sandboxed worker never opens a path itself (see
// SidecarRequestServicer.h, which calls this).
//
// allowPackageBasenameLookup: the downloaded-package image fallback. When an
// image reference does not resolve directly, its file name is looked up in
// exactly these directories, closest first:
//   1. beside the primary file
//   2. <primary dir>/texture
//   3. <primary dir>/textures
//   4. <parent>/texture
//   5. <parent>/textures
// The first directory with a match wins, and only after the same canonical-
// containment and size checks as any other sidecar. Names match exactly first,
// then with case, space/underscore, and jpg/jpeg/tif/tiff spelling normalized,
// plus material-role abbreviations (`_BaseColor`/`_B`, `_Normal`/`_N`,
// `_Roughness`/`_R`, `_Emissive`/`_E`, ...) and a trailing download annotation
// such as `_(Personalizado)`, so files renamed by a download site still match.
// The fallback is image-only:
// .bin buffers, .mtl files, and USD layers must stay where the document
// references them. Every syntactically unsafe reference is rejected before the
// lookup, so traversal/UNC/ADS/URL text keeps failing closed, and a name with
// wildcard characters is never used as a search pattern.
//
// Two input-boundary rejections happen before any path or extension work:
// an embedded NUL or other control character (anything < 0x20 or 0x7F) is
// hostile text, not something to trim (a silent trim would change which file
// is opened), and a byte sequence that is not valid UTF-8 is rejected rather
// than substituted. Both are checked on the raw bytes and again on the decoded
// text. The allowed extension set is chosen by `format`: a glTF may request a
// .bin buffer or an image, an OBJ a .mtl or an image, FBX an image, and USD a
// .usd/.usda/.usdc layer or an image (direct image assets are dependencies,
// not composition); STL/PLY/3MF/STEP reference nothing here (3MF and USDZ
// package entries stay in-archive and never become sidecar requests).

#include "import_broker/ImportSession.h"
#include "model_core/ImportError.h"
#include "platform/Win32Handle.h"

#include <cstdint>
#include <string>
#include <vector>

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
//
// additionalSearchRoots: user-chosen directories offered as a last resort
// after both the primary directory tree and the package lookup miss. These
// exist because a model opened from elsewhere can reference assets the user
// keeps in a separate folder. Each root is trusted only because the trusted
// UI picked it; the same canonical-containment rule still applies, so a
// reparse point inside a root cannot address anything outside that root. The
// root itself is canonicalized by opening its directory handle
// (GetFinalPathNameByHandleW, FILE_NAME_NORMALIZED), so an ordinary path the
// UI supplied compares correctly against the candidate's own canonical path;
// a root that cannot be canonicalized is skipped (fail closed), never compared
// as raw text.
// Only the reference's leaf file name is matched under each root (in the
// root itself and its `texture`/`textures` subfolders), never the authored
// relative path -- a user points at "the folder the assets are in", which
// need not mirror the document's own directory layout.
// format: the requesting import's own format (ImportSessionRequest::format),
// supplied by the trusted host. It selects the allowed sidecar extension set;
// the worker never chooses it. Defaults to Gltf so pure resolver callers keep
// the historical behavior, but production always passes the real format.
SidecarResolution ResolveSidecarPath(const std::wstring& primaryCanonicalPath,
                                      const std::string& relativeReferenceUtf8,
                                      uint64_t maxSidecarFileBytes,
                                      bool allowPackageBasenameLookup = false,
                                      const std::vector<std::wstring>& additionalSearchRoots = {},
                                      ImportFormat format = ImportFormat::Gltf);

} // namespace import_broker

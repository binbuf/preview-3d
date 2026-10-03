#pragma once

// Production servicing logic for a worker's mid-generation
// RequestSidecarFile message -- kept as one reusable, host-process-agnostic
// function (rather than inlined into D3D12ImportBridge.cpp) specifically so
// hostile-worker tests can exercise the exact production path, mirroring
// how import_broker::ValidateAndCopySection is already shared between the
// real app and every test.

#include "import_broker/ImportSession.h"
#include "model_core/ControlProtocol.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>
#include <windows.h>

namespace import_broker {

// primaryCanonicalPath: the .gltf's own already-canonicalized path
// (OpenAndCanonicalizeSourceFile's result). Decodes request's relative-path
// bytes, resolves via ResolveSidecarPath, and on success duplicates the
// resulting handle into workerProcess (DuplicateHandleIntoProcess). Once
// duplicated, the worker's handle is a fully independent reference to the
// same underlying kernel file object -- the host does not need to keep its
// own resolve-time handle open afterward, so this function's own handle is
// closed before returning either way.
//
// allowPackageBasenameLookup forwards ResolveSidecarPath's package-name
// fallback for the downloaded `<model>/source/...` + `<model>/textures/`
// layout; the trusted host enables it for local file imports, never from
// worker-provided text.
//
// additionalSearchRoots is forwarded to ResolveSidecarPath's user-chosen
// asset-root fallback (see that header). It is supplied only by the trusted
// host after the user picks a folder.
//
// format is the trusted host's own ImportSessionRequest::format, forwarded so
// ResolveSidecarPath applies the requesting format's allowed extension set.
// The worker never supplies it. Before any resolution, the raw reference bytes
// are rejected for an embedded NUL or other control character -- a first,
// cheap layer standing in front of the resolver, which repeats both that check
// and the UTF-8 validity check so a future caller cannot bypass them.
std::variant<model_core::SidecarFileReadyNotice, model_core::SidecarFileUnavailableNotice>
ServiceSidecarRequest(HANDLE workerProcess, const std::wstring& primaryCanonicalPath,
                      const model_core::RequestSidecarFileNotice& request, uint64_t maxSidecarFileBytes,
                      uint64_t remainingSourceBytes = UINT64_MAX,
                      bool allowPackageBasenameLookup = false,
                      const std::vector<std::wstring>& additionalSearchRoots = {},
                      ImportFormat format = ImportFormat::Gltf);

} // namespace import_broker

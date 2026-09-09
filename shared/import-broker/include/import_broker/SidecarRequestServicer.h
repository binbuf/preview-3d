#pragma once

// Production servicing logic for a worker's mid-generation
// RequestSidecarFile message -- kept as one reusable, host-process-agnostic
// function (rather than inlined into D3D12ImportBridge.cpp) specifically so
// hostile-worker tests can exercise the exact production path, mirroring
// how import_broker::ValidateAndCopySection is already shared between the
// real app and every test.

#include "model_core/ControlProtocol.h"

#include <cstdint>
#include <string>
#include <variant>
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
std::variant<model_core::SidecarFileReadyNotice, model_core::SidecarFileUnavailableNotice>
ServiceSidecarRequest(HANDLE workerProcess, const std::wstring& primaryCanonicalPath,
                       const model_core::RequestSidecarFileNotice& request, uint64_t maxSidecarFileBytes);

} // namespace import_broker

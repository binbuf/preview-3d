#pragma once

// Worker-side half of the mid-generation sidecar-request protocol (see
// model_core::ControlOpcode::RequestSidecarFile and
// import_broker/SidecarRequestServicer.h for the host side). Used only by
// the real-file glTF import path, when the .gltf being parsed turns out to
// reference an external buffer/image the worker cannot open itself.

#include "model_core/ImportError.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace import_worker {

class SidecarFileClient {
public:
    SidecarFileClient(HANDLE stdIn, HANDLE stdOut, uint64_t generationId) noexcept
        : stdIn_(stdIn)
        , stdOut_(stdOut)
        , generationId_(generationId)
    {
    }

    struct Result {
        std::optional<std::vector<std::byte>> bytes;
        model_core::ImportErrorCode errorCode = model_core::ImportErrorCode::None;
    };

    // Sends RequestSidecarFile and blocks for exactly one reply
    // (SidecarFileReady or SidecarFileUnavailable) before returning --
    // matches the channel's existing strictly-synchronous shape.
    Result RequestSidecarBytes(const std::string& relativePathUtf8);

private:
    HANDLE stdIn_;
    HANDLE stdOut_;
    uint64_t generationId_;
};

} // namespace import_worker

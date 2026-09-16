#pragma once

// Worker-side half of progressive delivery (see
// model_core::ControlOpcode::ChunkBatchReady/ChunkBatchConsumed and
// import_broker/ImportSession.h for the host side).
//
// A model larger than the output window cannot cross it in one write, so an
// adapter fills the window, hands it over through this sink, and only then
// reuses it for the next batch. The blocking wait in PublishBatch is not a
// convenience: the window is a *reused* buffer, and writing into it before
// the host says it is finished copying means racing the host for bytes it is
// still reading.
//
// The terminal batch does NOT go through here. It stays exactly what it
// always was -- the adapter leaves it in the window and its caller sends
// ChunksReady -- so a model that fits in one window produces byte-identical
// traffic to before progressive delivery existed, and every already-proven
// single-window path is untouched.
//
// Deliberately shaped like SidecarFileClient: same {stdIn, stdOut,
// generationId} construction, same strictly-synchronous one-reply-per-request
// discipline on the same control channel.

#include <cstdint>
#include "model_core/ControlProtocol.h"
#include <optional>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "model_core/ControlChannelIo.h"

namespace import_worker {

class ChunkBatchSink {
public:
    static void EnableCoarseProxy() { proxyEnabled_ = true; }
    static void EnableDetailService() { detailService_ = true; EnableCoarseProxy(); }
    bool DetailService() const { return detailService_; }
    const model_core::ChunkDescriptor* RequestedSource() const { return requested_ ? &*requested_ : nullptr; }
    bool AwaitDetail() {
        auto message = model_core::ReadControlMessage(stdIn_);
        if (!message || message->header.opcode != uint32_t(model_core::ControlOpcode::RequestDetail)
            || message->payload.size() != sizeof(model_core::DetailRequest)) return false;
        model_core::DetailRequest request;
        std::memcpy(&request, message->payload.data(), sizeof(request));
        if (request.generationId != generationId_ || request.source.lodLevel != model_core::kScanLod
            || !request.source.sourceRangeLength) return false;
        requested_ = request.source;
        return true;
    }
    bool ProxyEnabled() const { return proxyEnabled_; }
    bool Preview() const { return proxyEnabled_ && !scanStarted_; }
    void BeginScan() { scanStarted_ = true; }
    bool Refinement() const { return refinement_; }
    void BeginRefinement() { refinement_ = true; }
    static void SetDelayForTesting(unsigned milliseconds) { delayMs_ = milliseconds; }

    ChunkBatchSink(HANDLE stdIn, HANDLE stdOut, uint64_t generationId) noexcept
        : stdIn_(stdIn)
        , stdOut_(stdOut)
        , generationId_(generationId)
    {
    }

    // Announces the batch now sitting in the output window and blocks for the
    // host's ChunkBatchConsumed before returning. Returns false if the
    // handshake failed -- a rejected batch, or a host that is simply gone --
    // in which case the caller must abandon the generation rather than write
    // into the window again.
    bool Cancelled() const
    {
        DWORD available = 0;
        return !PeekNamedPipe(stdIn_, nullptr, 0, nullptr, &available, nullptr);
    }

    bool PublishBatch(uint32_t chunkCount, uint64_t sectionBytesWritten);

    // Batches successfully handed over so far, terminal one excluded.
    uint32_t BatchesPublished() const noexcept { return batchesPublished_; }

private:
    inline static bool proxyEnabled_ = false;
    inline static bool detailService_ = false;
    std::optional<model_core::ChunkDescriptor> requested_;
    bool refinement_ = false;
    bool scanStarted_ = false;
    inline static unsigned delayMs_ = 0;
    HANDLE stdIn_;
    HANDLE stdOut_;
    uint64_t generationId_;
    uint32_t batchesPublished_ = 0;
};

} // namespace import_worker

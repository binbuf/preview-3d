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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace import_worker {

class ChunkBatchSink {
public:
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
    bool PublishBatch(uint32_t chunkCount, uint64_t sectionBytesWritten);

    // Batches successfully handed over so far, terminal one excluded.
    uint32_t BatchesPublished() const noexcept { return batchesPublished_; }

private:
    HANDLE stdIn_;
    HANDLE stdOut_;
    uint64_t generationId_;
    uint32_t batchesPublished_ = 0;
};

} // namespace import_worker

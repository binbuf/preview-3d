#include "ChunkBatchSink.h"

#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"

#include <cstring>

namespace import_worker {

bool ChunkBatchSink::PublishBatch(uint32_t chunkCount, uint64_t sectionBytesWritten)
{
    model_core::ChunkBatchReadyNotice notice{};
    notice.generationId = generationId_;
    notice.batchIndex = batchesPublished_;
    notice.chunkCount = chunkCount;
    notice.sectionBytesWritten = sectionBytesWritten;

    if (!model_core::WriteControlMessage(stdOut_, model_core::ControlOpcode::ChunkBatchReady, &notice,
                                          sizeof(notice))) {
        return false;
    }

    // Blocks until the host has copied this batch out of the window. Anything
    // other than the matching ack -- a different opcode, a mismatched index,
    // or a closed pipe because the host rejected the batch and gave up -- is
    // a refusal to continue, never something to retry: the window's contents
    // are no longer anything this worker can reason about.
    auto received = model_core::ReadControlMessage(stdIn_);
    if (!received
        || received->header.opcode != static_cast<uint32_t>(model_core::ControlOpcode::ChunkBatchConsumed)
        || received->payload.size() != sizeof(model_core::ChunkBatchConsumedNotice)) {
        return false;
    }

    model_core::ChunkBatchConsumedNotice ack{};
    std::memcpy(&ack, received->payload.data(), sizeof(ack));
    if (ack.generationId != generationId_ || ack.batchIndex != notice.batchIndex) {
        return false;
    }

    ++batchesPublished_;
    return true;
}

} // namespace import_worker

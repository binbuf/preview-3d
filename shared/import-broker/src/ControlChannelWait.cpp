#include "import_broker/ControlChannelWait.h"

#include "model_core/ControlProtocol.h"

#include <utility>

namespace import_broker {

namespace {

constexpr DWORD kPollIntervalMs = 1;

// Blocks until at least `needed` bytes are buffered in the pipe, the
// deadline passes, or the write end goes away. A failed peek is how a dead
// worker is told apart from a merely slow one -- the same signal
// WorkerPool::WaitForReply already relied on.
ControlWaitOutcome WaitForBufferedBytes(HANDLE pipe, DWORD needed,
                                         std::chrono::steady_clock::time_point deadline,
                                         const std::function<bool()>& isCancelled)
{
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            return ControlWaitOutcome::Eof;
        }
        if (available >= needed) {
            return ControlWaitOutcome::Ready;
        }
        if (isCancelled && isCancelled()) {
            return ControlWaitOutcome::Cancelled;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return ControlWaitOutcome::TimedOut;
        }
        Sleep(kPollIntervalMs);
    }
}

} // namespace

ControlWaitOutcome ReadControlMessageBounded(HANDLE pipe, std::chrono::milliseconds timeout,
                                              model_core::ReceivedControlMessage& outMessage,
                                              const std::function<bool()>& isCancelled)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    ControlWaitOutcome outcome
        = WaitForBufferedBytes(pipe, sizeof(model_core::ControlMessageHeader), deadline, isCancelled);
    if (outcome != ControlWaitOutcome::Ready) {
        return outcome;
    }

    // Peek the header without consuming it, so the payload wait below can
    // size itself before anything leaves the pipe. Consuming first would
    // reintroduce exactly the stall this function exists to bound.
    model_core::ControlMessageHeader header{};
    DWORD peeked = 0;
    if (!PeekNamedPipe(pipe, &header, sizeof(header), &peeked, nullptr, nullptr)
        || peeked != sizeof(header)) {
        return ControlWaitOutcome::Eof;
    }
    if (header.payloadSize > model_core::kMaxControlPayloadBytes) {
        // Refuse before waiting on a length the protocol forbids -- the same
        // bound ReadControlMessage enforces, applied early so a bogus size
        // cannot spend the whole timeout.
        return ControlWaitOutcome::Eof;
    }

    outcome = WaitForBufferedBytes(pipe, sizeof(header) + header.payloadSize, deadline, isCancelled);
    if (outcome != ControlWaitOutcome::Ready) {
        return outcome;
    }

    // The complete message is buffered, so this read cannot block.
    auto received = model_core::ReadControlMessage(pipe);
    if (!received) {
        return ControlWaitOutcome::Eof;
    }
    outMessage = std::move(*received);
    return ControlWaitOutcome::Ready;
}

} // namespace import_broker

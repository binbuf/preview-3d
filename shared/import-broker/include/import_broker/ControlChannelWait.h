#pragma once

// A bounded read of one control message from a worker's reply pipe.
//
// model_core::ReadControlMessage is a blocking ReadFile with no timeout, so
// a worker that neither replies nor exits blocks its caller forever. A
// worker that *crashes* unblocks it (broken pipe); a worker that *hangs*
// does not -- and the existing --hang worker mode reproduces exactly that.
//
// This lives in its own file rather than in WorkerPool.cpp (whose
// WaitForReply first grew this shape) because Preview3D.vcxproj deliberately
// does not compile WorkerPool.cpp, and the product's one-shot import path
// needs the same bound.
//
// Two differences from WorkerPool::WaitForReply's original loop, both
// deliberate:
//
//  - The deadline covers the *whole message*, not just its first byte. The
//    original committed to an unbounded ReadControlMessage as soon as any
//    byte was available, so a worker that wrote a message header and then
//    stalled mid-payload still hung past its timeout.
//  - Time comes from steady_clock rather than accumulating the nominal poll
//    interval. Sleep(1) really takes 1-15 ms depending on timer resolution,
//    so counting `elapsed += 1` per iteration drifts long by up to 15x.

#include "model_core/ControlChannelIo.h"

#include <windows.h>

#include <chrono>
#include <functional>

namespace import_broker {

enum class ControlWaitOutcome {
    Ready,     // outMessage holds a complete, size-checked message
    TimedOut,  // deadline passed with the message still incomplete
    Cancelled, // isCancelled asked us to stop waiting
    Eof,       // write end gone, or a declared payload size the protocol forbids
};

// isCancelled, when supplied, is polled between waits so a superseded or
// closing generation stops waiting promptly instead of holding the thread
// for the full timeout. It must be cheap and must not block: it runs inside
// the poll loop. Empty means "never cancelled".
ControlWaitOutcome ReadControlMessageBounded(HANDLE pipe, std::chrono::milliseconds timeout,
                                              model_core::ReceivedControlMessage& outMessage,
                                              const std::function<bool()>& isCancelled = {});

} // namespace import_broker

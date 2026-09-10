#include "AttackModes.h"

#include <cstring>

namespace {

bool ArgEquals(const char* arg, const char* value)
{
    return std::strcmp(arg, value) == 0;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 2) {
        return 1;
    }

    if (ArgEquals(argv[1], "--mutate-after-ready")) {
        return hostile_worker::RunMutateAfterReady();
    }

    if (ArgEquals(argv[1], "--replay-generation")) {
        return hostile_worker::RunReplayGeneration();
    }

    if (ArgEquals(argv[1], "--lie-offset")) {
        return hostile_worker::RunLieOffset();
    }

    if (ArgEquals(argv[1], "--lie-layout")) {
        return hostile_worker::RunLieLayout();
    }

    // Progressive-delivery attacks; see AttackModes.h.
    if (ArgEquals(argv[1], "--batches-honest")) {
        return hostile_worker::RunHonestBatches();
    }

    if (ArgEquals(argv[1], "--batches-replay-index")) {
        return hostile_worker::RunReplayBatchIndex();
    }

    if (ArgEquals(argv[1], "--batches-skip-index")) {
        return hostile_worker::RunSkipBatchIndex();
    }

    if (ArgEquals(argv[1], "--batches-reuse-chunk-id")) {
        return hostile_worker::RunReuseChunkIdAcrossBatches();
    }

    if (ArgEquals(argv[1], "--batches-unbounded")) {
        return hostile_worker::RunUnboundedBatches();
    }

    if (ArgEquals(argv[1], "--batches-write-before-ack")) {
        return hostile_worker::RunWriteBeforeAck();
    }

    if (ArgEquals(argv[1], "--batches-after-terminal")) {
        return hostile_worker::RunBatchAfterTerminal();
    }

    return 1;
}

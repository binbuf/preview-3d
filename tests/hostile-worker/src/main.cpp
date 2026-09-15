#include "AttackModes.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "model_core/ImportError.h"
#include <windows.h>

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
    if (std::strncmp(argv[1], "--error-", 8) == 0) {
        auto request = model_core::ReadControlMessage(GetStdHandle(STD_INPUT_HANDLE));
        if (!request || request->payload.size() < 8) return 1;
        model_core::GenerationErrorNotice notice{};
        std::memcpy(&notice.generationId, request->payload.data(), 8);
        notice.errorCode = uint32_t(model_core::ImportErrorCode::MalformedData);
        if (ArgEquals(argv[1], "--error-stale")) ++notice.generationId;
        if (ArgEquals(argv[1], "--error-unknown")) notice.errorCode = UINT32_MAX;
        if (ArgEquals(argv[1], "--error-none")) notice.errorCode = 0;
        if (ArgEquals(argv[1], "--error-phase")) notice.reserved0 = 4;
        if (ArgEquals(argv[1], "--error-cancel")) notice.errorCode = uint32_t(model_core::ImportErrorCode::Cancelled);
        model_core::WriteControlMessage(GetStdHandle(STD_OUTPUT_HANDLE), model_core::ControlOpcode::GenerationError,
            &notice, ArgEquals(argv[1], "--error-short") ? 8 : sizeof(notice));
        return 0;
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

    if (ArgEquals(argv[1], "--texture-missing-root")) return hostile_worker::RunTextureAttack(0);
    if (ArgEquals(argv[1], "--texture-semantic-change")) return hostile_worker::RunTextureAttack(1);
    if (ArgEquals(argv[1], "--texture-repeat-refinement")) return hostile_worker::RunTextureAttack(2);
    if (ArgEquals(argv[1], "--texture-warning-limit")) return hostile_worker::RunTextureAttack(3);
    if (ArgEquals(argv[1], "--texture-aggregate-expansion")) return hostile_worker::RunTextureAttack(4);
    if (ArgEquals(argv[1], "--status-flags")) return hostile_worker::RunTextureAttack(5);
    if (ArgEquals(argv[1], "--status-count")) return hostile_worker::RunTextureAttack(6);
    if (ArgEquals(argv[1], "--status-reserved")) return hostile_worker::RunTextureAttack(7);
    if (ArgEquals(argv[1], "--status-duplicate")) return hostile_worker::RunTextureAttack(8);
    if (ArgEquals(argv[1], "--catalog-forward")) return hostile_worker::RunCatalogBatches(0);
    if (ArgEquals(argv[1], "--catalog-wrong-image")) return hostile_worker::RunCatalogBatches(1);
    if (ArgEquals(argv[1], "--catalog-missing-image")) return hostile_worker::RunCatalogBatches(2);
    if (ArgEquals(argv[1], "--catalog-wrong-material")) return hostile_worker::RunCatalogBatches(3);
    if (ArgEquals(argv[1], "--origin-nan")) return hostile_worker::RunMetadataAttack(0);
    if (ArgEquals(argv[1], "--origin-inf")) return hostile_worker::RunMetadataAttack(1);
    if (ArgEquals(argv[1], "--bounds-fabricated")) return hostile_worker::RunMetadataAttack(2);
    if (ArgEquals(argv[1], "--bounds-nan")) return hostile_worker::RunMetadataAttack(3);
    if (ArgEquals(argv[1], "--bounds-reversed")) return hostile_worker::RunMetadataAttack(4);
    if (ArgEquals(argv[1], "--metadata-enum")) return hostile_worker::RunMetadataAttack(5);
    if (ArgEquals(argv[1], "--metadata-stale")) return hostile_worker::RunMetadataAttack(6);
    if (ArgEquals(argv[1], "--protocol-old")) return hostile_worker::RunMetadataAttack(7);
    if (ArgEquals(argv[1], "--metadata-changed")) return hostile_worker::RunMetadataAttack(8);
    if (ArgEquals(argv[1], "--origin-distant-batch")) return hostile_worker::RunMetadataAttack(11);
    if (ArgEquals(argv[1], "--position-nan")) return hostile_worker::RunMetadataAttack(9);
    if (ArgEquals(argv[1], "--position-inf")) return hostile_worker::RunMetadataAttack(10);

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

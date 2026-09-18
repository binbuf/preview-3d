#include "ThreeMfImportWorker.h"
#include "ThreeMfOpcPreflight.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/MappedFile.h"
#include "model_core/TierALimits.h"
#include "platform/Win32Handle.h"
#include <cstring>

namespace import_worker {
namespace {
bool Fail(HANDLE out, uint64_t generation, model_core::ImportErrorCode code) {
    model_core::GenerationErrorNotice notice{}; notice.generationId = generation; notice.errorCode = uint32_t(code);
    return model_core::WriteControlMessage(out, model_core::ControlOpcode::GenerationError, &notice, sizeof(notice));
}
}
bool HandleThreeMfImportFileRequest(HANDLE out, const model_core::ParseThreeMfFileRequest& request) {
    if (request.requestFlags) return Fail(out, request.generationId, model_core::ImportErrorCode::ImportProtocolViolation);
    HANDLE cancel = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.cancellationEventHandleValue));
    if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) return Fail(out, request.generationId, model_core::ImportErrorCode::Cancelled);
    HANDLE raw = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sourceFileHandleValue));
    auto mapped = model_core::MappedFile::FromHandle(platform::Win32Handle(raw));
    if (!mapped.file) return Fail(out, request.generationId, model_core::ImportErrorCode::InternalImporterFailure);
    if (mapped.file->SizeBytes() > model_core::kTierBPrimarySourceBytes) return Fail(out, request.generationId, model_core::ImportErrorCode::PrimarySourceLimit);
    std::wstring error; auto source = mapped.file->MapWhole(error);
    if (!source) return Fail(out, request.generationId, model_core::ImportErrorCode::InternalImporterFailure);
    const auto result = InspectThreeMfOpc(source.Bytes(), nullptr, {}, [cancel] { return cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0; });
    if (result == ThreeMfOpcError::Cancelled) return Fail(out, request.generationId, model_core::ImportErrorCode::Cancelled);
    if (result == ThreeMfOpcError::UnsupportedRequiredFeature) return Fail(out, request.generationId, model_core::ImportErrorCode::UnsupportedRequiredFeature);
    if (result != ThreeMfOpcError::None) return Fail(out, request.generationId, model_core::ImportErrorCode::ArchiveLimit);
    // 3MF-003 owns lib3mf construction and normalized scene output. A package
    // which crosses this control route has passed the same bounded boundary it
    // will use there; it cannot be mistaken for a successfully imported scene.
    return Fail(out, request.generationId, model_core::ImportErrorCode::UnsupportedRequiredFeature);
}
int RunThreeMfImport() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    auto message = model_core::ReadControlMessage(in);
    if (!message || message->header.opcode != uint32_t(model_core::ControlOpcode::StartThreeMfImportFromFile) || message->payload.size() != sizeof(model_core::ParseThreeMfFileRequest)) return 1;
    model_core::ParseThreeMfFileRequest request{}; std::memcpy(&request, message->payload.data(), sizeof(request));
    return HandleThreeMfImportFileRequest(out, request) ? 0 : 1;
}
}

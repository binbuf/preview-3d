#include "WorkerRequestDispatch.h"

#include "GenerationWorker.h"
#include "GltfImportWorker.h"
#include "StlImportWorker.h"

#include "model_core/ControlChannelIo.h"

#include <cstring>

namespace import_worker {

DispatchOutcome DispatchOneRequest(HANDLE stdIn, HANDLE stdOut)
{
    auto received = model_core::ReadControlMessage(stdIn);
    if (!received) {
        return DispatchOutcome::ProtocolError; // EOF or malformed framing
    }

    if (received->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::Shutdown)) {
        return DispatchOutcome::Shutdown;
    }

    if (received->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::StartGeneration)
        && received->payload.size() == sizeof(model_core::StartGenerationRequest)) {
        model_core::StartGenerationRequest request{};
        std::memcpy(&request, received->payload.data(), sizeof(request));
        HandleStartGeneration(stdOut, request);
        return DispatchOutcome::Continue;
    }

    if (received->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::StartGltfImport)
        && received->payload.size() == sizeof(model_core::ParseGltfRequest)) {
        model_core::ParseGltfRequest request{};
        std::memcpy(&request, received->payload.data(), sizeof(request));
        HandleGltfImportRequest(stdOut, request);
        return DispatchOutcome::Continue;
    }

    if (received->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::StartGltfImportFromFile)
        && received->payload.size() == sizeof(model_core::ParseGltfFileRequest)) {
        model_core::ParseGltfFileRequest request{};
        std::memcpy(&request, received->payload.data(), sizeof(request));
        HandleGltfImportFileRequest(stdOut, request);
        return DispatchOutcome::Continue;
    }

    if (received->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::StartStlImportFromFile)
        && received->payload.size() == sizeof(model_core::ParseStlFileRequest)) {
        model_core::ParseStlFileRequest request{};
        std::memcpy(&request, received->payload.data(), sizeof(request));
        HandleStlImportFileRequest(stdOut, request);
        return DispatchOutcome::Continue;
    }

    return DispatchOutcome::ProtocolError; // unrecognized opcode/size
}

int RunPoolMode()
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdIn == nullptr || stdIn == INVALID_HANDLE_VALUE || stdOut == nullptr
        || stdOut == INVALID_HANDLE_VALUE) {
        return 1;
    }

    for (;;) {
        DispatchOutcome outcome = DispatchOneRequest(stdIn, stdOut);
        if (outcome == DispatchOutcome::Shutdown) {
            return 0;
        }
        if (outcome == DispatchOutcome::ProtocolError) {
            return 1;
        }
        // Continue: loop for the next request.
    }
}

} // namespace import_worker

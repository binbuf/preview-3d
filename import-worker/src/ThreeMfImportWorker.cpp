#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ThreeMfImportWorker.h"

#include "ChunkBatchSink.h"
#include "ThreeMfAdapter.h"
#include "ThreeMfDisplayProperties.h"
#include "ThreeMfOpcPreflight.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/MappedFile.h"
#include "model_core/TierALimits.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <Bindings/Cpp/lib3mf_implicit.hpp>

#include <cstring>
#include <limits>
#include <new>

namespace import_worker {
namespace {

using model_core::ImportErrorCode;

bool ReportError(HANDLE out, uint64_t generation, ImportErrorCode code) {
    model_core::GenerationErrorNotice notice{};
    notice.generationId = generation; notice.errorCode = uint32_t(code);
    return model_core::WriteControlMessage(out, model_core::ControlOpcode::GenerationError,
                                           &notice, sizeof(notice));
}

bool ReportOutcome(HANDLE out, uint64_t generation, const ThreeMfImportOutcome& outcome) {
    if (const auto* failure = std::get_if<ThreeMfImportFailure>(&outcome))
        return ReportError(out, generation, failure->code);
    const auto& result = std::get<ThreeMfImportResult>(outcome);
    model_core::ChunksReadyNotice notice{};
    notice.generationId = generation; notice.chunkCount = result.chunkCount;
    notice.sectionBytesWritten = result.sectionBytesWritten;
    return model_core::WriteControlMessage(out, model_core::ControlOpcode::ChunksReady,
                                           &notice, sizeof(notice));
}

struct CallbackSource {
    HANDLE file = nullptr;
    HANDLE cancellation = nullptr;
    BY_HANDLE_FILE_INFORMATION identity{};
    bool ioFailure = false;
};

bool SameSource(const CallbackSource& source) {
    BY_HANDLE_FILE_INFORMATION current{};
    return GetFileInformationByHandle(source.file, &current)
        && current.dwVolumeSerialNumber == source.identity.dwVolumeSerialNumber
        && current.nFileIndexHigh == source.identity.nFileIndexHigh
        && current.nFileIndexLow == source.identity.nFileIndexLow
        && current.nFileSizeHigh == source.identity.nFileSizeHigh
        && current.nFileSizeLow == source.identity.nFileSizeLow
        && CompareFileTime(&current.ftLastWriteTime, &source.identity.ftLastWriteTime) == 0;
}

void ReadCallback(Lib3MF_uint64 destinationValue, Lib3MF_uint64 requested, Lib3MF_pvoid userData) {
    auto& source = *static_cast<CallbackSource*>(userData);
    auto* destination = reinterpret_cast<std::byte*>(static_cast<uintptr_t>(destinationValue));
    uint64_t total = 0;
    if (!SameSource(source)) source.ioFailure = true;
    while (!source.ioFailure && total < requested) {
        const DWORD size = static_cast<DWORD>((std::min)(requested - total, uint64_t(MAXDWORD)));
        DWORD read = 0;
        if (!ReadFile(source.file, destination + total, size, &read, nullptr) || read != size) {
            source.ioFailure = true; break;
        }
        total += read;
    }
    // lib3mf callbacks cannot report failure. Deterministic fill prevents
    // uninitialized bytes becoming parser input; ioFailure denies acceptance.
    if (total < requested)
        std::memset(destination + total, 0, static_cast<size_t>(requested - total));
}

void SeekCallback(Lib3MF_uint64 position, Lib3MF_pvoid userData) {
    auto& source = *static_cast<CallbackSource*>(userData);
    LARGE_INTEGER target{}; target.QuadPart = static_cast<LONGLONG>(position);
    if (position > uint64_t((std::numeric_limits<LONGLONG>::max)())
        || !SetFilePointerEx(source.file, target, nullptr, FILE_BEGIN)) source.ioFailure = true;
}

void ProgressCallback(bool* abort, Lib3MF_double, Lib3MF::eProgressIdentifier, Lib3MF_pvoid userData) {
    const auto& source = *static_cast<CallbackSource*>(userData);
    *abort = source.ioFailure || (source.cancellation
        && WaitForSingleObject(source.cancellation, 0) == WAIT_OBJECT_0);
}

} // namespace

bool HandleThreeMfImportFileRequest(HANDLE in, HANDLE out, const model_core::ParseThreeMfFileRequest& request) {
    if (request.requestFlags) return ReportError(out, request.generationId, ImportErrorCode::ImportProtocolViolation);
    HANDLE cancellation = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.cancellationEventHandleValue));
    if (cancellation && WaitForSingleObject(cancellation, 0) == WAIT_OBJECT_0)
        return ReportError(out, request.generationId, ImportErrorCode::Cancelled);

    HANDLE raw = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sourceFileHandleValue));
    HANDLE callbackRaw = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), raw, GetCurrentProcess(), &callbackRaw, GENERIC_READ, FALSE, 0))
        return ReportError(out, request.generationId, ImportErrorCode::InternalImporterFailure);
    platform::Win32Handle callbackFile(callbackRaw);
    auto opened = model_core::MappedFile::FromHandle(platform::Win32Handle(raw));
    if (!opened.file) return ReportError(out, request.generationId, ImportErrorCode::InternalImporterFailure);
    if (opened.file->SizeBytes() > model_core::kTierBPrimarySourceBytes)
        return ReportError(out, request.generationId, ImportErrorCode::PrimarySourceLimit);
    std::wstring mapError; auto source = opened.file->MapWhole(mapError);
    if (!source) return ReportError(out, request.generationId, ImportErrorCode::InternalImporterFailure);
    ThreeMfOpcPackage package;
    const auto preflight = InspectThreeMfOpc(source.Bytes(), &package, {}, [cancellation] {
        return cancellation && WaitForSingleObject(cancellation, 0) == WAIT_OBJECT_0;
    });
    if (preflight == ThreeMfOpcError::Cancelled) return ReportError(out, request.generationId, ImportErrorCode::Cancelled);
    if (preflight == ThreeMfOpcError::UnsupportedRequiredFeature)
        return ReportError(out, request.generationId, ImportErrorCode::UnsupportedRequiredFeature);
    if (preflight != ThreeMfOpcError::None) return ReportError(out, request.generationId, ImportErrorCode::ArchiveLimit);

    ThreeMfDisplayCatalog displayCatalog;
    const auto displayScan = ScanThreeMfDisplayProperties(
        source.Bytes(), package, displayCatalog, [cancellation] {
            return cancellation && WaitForSingleObject(cancellation, 0) == WAIT_OBJECT_0;
        });
    if (displayScan != ImportErrorCode::None)
        return ReportError(out, request.generationId, displayScan);

    platform::Win32Handle outputSection(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sectionHandleValue)));
    auto output = platform::MappedView::Map(outputSection.get(), FILE_MAP_WRITE | FILE_MAP_READ,
                                            static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!output) return ReportError(out, request.generationId, ImportErrorCode::InternalImporterFailure);
    CallbackSource callback{}; callback.file = callbackFile.get(); callback.cancellation = cancellation;
    if (!GetFileInformationByHandle(callback.file, &callback.identity))
        return ReportError(out, request.generationId, ImportErrorCode::InternalImporterFailure);
    const uint64_t size = (uint64_t(callback.identity.nFileSizeHigh) << 32) | callback.identity.nFileSizeLow;
    if (size != opened.file->SizeBytes()) return ReportError(out, request.generationId, ImportErrorCode::FileChanged);

    ChunkBatchSink sink(in, out, request.generationId, request.requestFlags, cancellation);
    ThreeMfImportOptions options;
    options.isCancelled = [&sink] { return sink.Cancelled(); };
    options.displayCatalog = &displayCatalog;
    try {
        auto wrapper = Lib3MF::CWrapper::loadLibrary();
        auto model = wrapper->CreateModel();
        auto reader = model->QueryReader("3mf");
        // lib3mf 2.5 strict mode rejects both standardized display-property
        // resources and ordinary packages emitted by current slicers. The
        // product-owned OPC/XML boundary and the adapter validate every value
        // that can affect normalized output, so use the library's compatible
        // reader mode and continue to fail closed at those owned boundaries.
        reader->SetStrictModeActive(false);
        reader->SetProgressCallback(ProgressCallback, &callback);
        reader->ReadFromCallback(ReadCallback, size, SeekCallback, &callback);
        if (callback.ioFailure) return ReportError(out, request.generationId,
            options.Cancelled() ? ImportErrorCode::Cancelled : ImportErrorCode::FileChanged);
        return ReportOutcome(out, request.generationId,
            ImportThreeMf(model, output.bytes(), request.generationId, request.maxChunkCount, &sink, options));
    } catch (const Lib3MF::ELib3MFException& error) {
        return ReportError(out, request.generationId,
            error.getErrorCode() == LIB3MF_ERROR_CALCULATIONABORTED || options.Cancelled()
                ? ImportErrorCode::Cancelled : callback.ioFailure ? ImportErrorCode::FileChanged
                : ImportErrorCode::MalformedData);
    } catch (const std::bad_alloc&) {
        return ReportError(out, request.generationId, ImportErrorCode::OutOfMemory);
    } catch (...) {
        return ReportError(out, request.generationId, ImportErrorCode::InternalImporterFailure);
    }
}

int RunThreeMfImport() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    auto message = model_core::ReadControlMessage(in);
    if (!message || message->header.opcode != uint32_t(model_core::ControlOpcode::StartThreeMfImportFromFile)
        || message->payload.size() != sizeof(model_core::ParseThreeMfFileRequest)) return 1;
    model_core::ParseThreeMfFileRequest request{};
    std::memcpy(&request, message->payload.data(), sizeof(request));
    return HandleThreeMfImportFileRequest(in, out, request) ? 0 : 1;
}

} // namespace import_worker

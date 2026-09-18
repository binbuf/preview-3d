#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "UsdImportWorker.h"

#include "UsdZipPreflight.h"
#include "model_core/Checksum.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/MappedFile.h"
#include "model_core/TierALimits.h"
#include "model_core/GeometryBounds.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include "tinyusdz.hh"

#include <cstring>
#include <span>

namespace import_worker {
namespace {

using model_core::ImportErrorCode;
using model_core::SourceFormatId;

SourceFormatId DetectUsdEncoding(std::span<const std::byte> source)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(source.data());
    if (tinyusdz::IsUSDA(bytes, source.size())) return SourceFormatId::Usda;
    if (tinyusdz::IsUSDC(bytes, source.size())) return SourceFormatId::Usdc;
    if (tinyusdz::IsUSDZ(bytes, source.size())) return SourceFormatId::Usdz;
    return SourceFormatId::Unknown;
}

bool EncodingMatchesRequest(SourceFormatId detected, uint32_t flags)
{
    const uint32_t expected = flags & model_core::kImportRequestUsdExpectedMask;
    if (expected == 0) return true; // `.usd`: bytes are authoritative.
    if ((expected & (expected - 1)) != 0) return false;
    return (expected == model_core::kImportRequestUsdExpectedUsda && detected == SourceFormatId::Usda)
        || (expected == model_core::kImportRequestUsdExpectedUsdc && detected == SourceFormatId::Usdc)
        || (expected == model_core::kImportRequestUsdExpectedUsdz && detected == SourceFormatId::Usdz);
}

bool ReportError(HANDLE stdOut, uint64_t generationId, ImportErrorCode code)
{
    model_core::GenerationErrorNotice notice{};
    notice.generationId = generationId;
    notice.errorCode = uint32_t(code);
    notice.reserved0 = uint32_t(model_core::ImportFailurePhase::Geometry);
    model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::GenerationError,
                                    &notice, sizeof(notice));
    return false;
}

bool WriteContractResult(std::span<std::byte> output, uint64_t generationId,
                         uint32_t maxChunkCount, SourceFormatId format,
                         model_core::ChunksReadyNotice& notice)
{
    using namespace model_core;
    constexpr uint64_t payloadOffset = kSectionHeaderSize + kChunkDescriptorSize;
    constexpr uint64_t sectionLength = payloadOffset + sizeof(VertexPositionOnlyF32);
    if (maxChunkCount < 1 || sectionLength > output.size()) return false;

    // Metadata-only sections are rejected by the product broker as empty.
    // This one-point contract marker keeps the USD-003 route end-to-end
    // testable without pretending to normalize source geometry before USD-004.
    const VertexPositionOnlyF32 marker{};
    std::memcpy(output.data() + payloadOffset, &marker, sizeof(marker));

    ChunkDescriptor descriptor{};
    descriptor.normalizedRangeOffset = payloadOffset;
    descriptor.sourceRangeLength = 1;
    descriptor.normalizedRangeLength = sizeof(marker);
    descriptor.topology = ChunkTopology::PointList;
    descriptor.vertexCount = 1;
    descriptor.vertexLayoutId = uint32_t(VertexLayoutId::PositionOnly_F32);
    descriptor.chunkId = 1;
    descriptor.byteSize = sizeof(marker);
    SetLocalBounds(descriptor, output.subspan(payloadOffset, sizeof(marker)));
    descriptor.chunkChecksum = WireChecksum64(output.subspan(payloadOffset, sizeof(marker)));
    std::memcpy(output.data() + kSectionHeaderSize, &descriptor, sizeof(descriptor));

    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = generationId;
    header.sectionLength = sectionLength;
    header.chunkCount = 1;
    header.scene.generationId = generationId;
    header.scene.format = format;
    // USD's authored/fallback values are always concrete. The contract route
    // uses the USD defaults; USD-004 will report the parsed X/Y/Z and units.
    header.scene.upAxis = UpAxisId::Y;
    header.scene.metersPerUnit = 0.01;
    header.scene.meshCount = 1;
    header.sectionChecksum = WireChecksum64(
        output.subspan(kSectionHeaderSize, sectionLength - kSectionHeaderSize));
    std::memcpy(output.data(), &header, sizeof(header));

    notice.generationId = generationId;
    notice.chunkCount = 1;
    notice.sectionBytesWritten = sectionLength;
    return true;
}

} // namespace

bool HandleUsdImportFileRequest(HANDLE, HANDLE stdOut,
                                const model_core::ParseUsdFileRequest& request)
{
    const uint32_t allowedFlags = model_core::kImportRequestUsdExpectedMask;
    if ((request.requestFlags & ~allowedFlags) != 0)
        return ReportError(stdOut, request.generationId, ImportErrorCode::ImportProtocolViolation);
    if (request.cancellationEventHandleValue) {
        HANDLE cancellation = reinterpret_cast<HANDLE>(
            static_cast<uintptr_t>(request.cancellationEventHandleValue));
        if (WaitForSingleObject(cancellation, 0) == WAIT_OBJECT_0)
            return ReportError(stdOut, request.generationId, ImportErrorCode::Cancelled);
    }

    HANDLE rawFile = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sourceFileHandleValue));
    auto opened = model_core::MappedFile::FromHandle(platform::Win32Handle(rawFile));
    if (!opened.file)
        return ReportError(stdOut, request.generationId, ImportErrorCode::InternalImporterFailure);
    if (opened.file->SizeBytes() > model_core::kTierBPrimarySourceBytes)
        return ReportError(stdOut, request.generationId, ImportErrorCode::PrimarySourceLimit);
    std::wstring mapError;
    auto source = opened.file->MapWhole(mapError);
    if (!source)
        return ReportError(stdOut, request.generationId, ImportErrorCode::InternalImporterFailure);

    const SourceFormatId format = DetectUsdEncoding(source.Bytes());
    if (format == SourceFormatId::Unknown)
        return ReportError(stdOut, request.generationId, ImportErrorCode::MalformedData);
    if (!EncodingMatchesRequest(format, request.requestFlags))
        return ReportError(stdOut, request.generationId, ImportErrorCode::UnsupportedEncoding);
    if (format == SourceFormatId::Usdz && PreflightUsdz(source.Bytes()) != UsdzPreflightError::None)
        return ReportError(stdOut, request.generationId, ImportErrorCode::ArchiveLimit);

    platform::Win32Handle outputSection(reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(request.sectionHandleValue)));
    auto output = platform::MappedView::Map(outputSection.get(), FILE_MAP_WRITE | FILE_MAP_READ,
                                            static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!output)
        return ReportError(stdOut, request.generationId, ImportErrorCode::InternalImporterFailure);

    model_core::ChunksReadyNotice notice{};
    if (!WriteContractResult(output.bytes(), request.generationId, request.maxChunkCount,
                             format, notice))
        return ReportError(stdOut, request.generationId, ImportErrorCode::ResourceLimit);
    model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::ChunksReady,
                                    &notice, sizeof(notice));
    return true;
}

int RunUsdImport()
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE), stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!stdIn || stdIn == INVALID_HANDLE_VALUE || !stdOut || stdOut == INVALID_HANDLE_VALUE)
        return 1;
    auto message = model_core::ReadControlMessage(stdIn);
    if (!message
        || message->header.opcode != uint32_t(model_core::ControlOpcode::StartUsdImportFromFile)
        || message->payload.size() != sizeof(model_core::ParseUsdFileRequest)) return 1;
    model_core::ParseUsdFileRequest request{};
    std::memcpy(&request, message->payload.data(), sizeof(request));
    return HandleUsdImportFileRequest(stdIn, stdOut, request) ? 0 : 1;
}

} // namespace import_worker

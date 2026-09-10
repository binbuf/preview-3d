#include "AttackModes.h"

#include "SyntheticSceneGenerator.h"

#include "model_core/Checksum.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "platform/MappedView.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <variant>

namespace hostile_worker {

namespace {

// Delay before RunMutateAfterReady corrupts the section, comfortably longer
// than any plausible host validation pass over the ~1KB honest fixture
// (memcpy + FNV-1a64 + a handful of small vector allocations -- low
// single-digit milliseconds even pessimistically). 2+ orders of magnitude
// of headroom, consistent with this codebase's existing timeout precedents
// (SandboxLaunchTests.cpp's 500ms/5000ms waits).
constexpr DWORD kCorruptionDelayMs = 300;

struct RequestAndSection {
    model_core::StartGenerationRequest request;
    platform::MappedView view;
};

// Common preamble shared by all four attack modes: read one
// StartGenerationRequest from the inherited stdin pipe and map the
// inherited shared section. Deliberately not calling into
// GenerationWorker.cpp -- see AttackModes.h.
std::optional<RequestAndSection> ReadRequestAndMapSection()
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (stdIn == nullptr || stdIn == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    auto received = model_core::ReadControlMessage(stdIn);
    if (!received
        || received->header.opcode != static_cast<uint32_t>(model_core::ControlOpcode::StartGeneration)
        || received->payload.size() != sizeof(model_core::StartGenerationRequest)) {
        return std::nullopt;
    }

    model_core::StartGenerationRequest request{};
    std::memcpy(&request, received->payload.data(), sizeof(request));

    HANDLE section = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sectionHandleValue));
    auto view = platform::MappedView::Map(section, FILE_MAP_WRITE | FILE_MAP_READ,
                                           static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!view) {
        return std::nullopt;
    }

    RequestAndSection result;
    result.request = request;
    result.view = std::move(view);
    return result;
}

bool SendChunksReady(uint64_t generationId, uint32_t chunkCount, uint64_t sectionBytesWritten)
{
    HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdOut == nullptr || stdOut == INVALID_HANDLE_VALUE) {
        return false;
    }

    model_core::ChunksReadyNotice notice{};
    notice.generationId = generationId;
    notice.chunkCount = chunkCount;
    notice.sectionBytesWritten = sectionBytesWritten;
    return model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::ChunksReady, &notice,
                                            sizeof(notice));
}

// Hand-builds a single-PointList-chunk, single-vertex section directly into
// `destination`, honest in every respect except whatever the caller
// overrides via `corrupt` after the honest fields are set but before
// checksums are computed over what was actually written -- matching
// SyntheticSceneGenerator's "header written last" ordering.
template <typename CorruptFn>
uint64_t BuildLyingSingleChunkSection(std::span<std::byte> destination, uint64_t generationId,
                                       CorruptFn corrupt)
{
    using namespace model_core;

    VertexPositionOnlyF32 vertex{ 7.0f, 8.0f, 9.0f };

    uint64_t payloadOffset = kSectionHeaderSize + kChunkDescriptorSize;
    uint64_t sectionLength = payloadOffset + sizeof(vertex);

    std::memcpy(destination.data() + payloadOffset, &vertex, sizeof(vertex));

    ChunkDescriptor descriptor{};
    descriptor.normalizedRangeOffset = payloadOffset;
    descriptor.normalizedRangeLength = sizeof(vertex);
    descriptor.topology = ChunkTopology::PointList;
    descriptor.indexCount = 0;
    descriptor.vertexCount = 1;
    descriptor.vertexLayoutId = static_cast<uint32_t>(VertexLayoutId::PositionOnly_F32);
    descriptor.lodLevel = 0;
    descriptor.chunkId = 1;
    descriptor.byteSize = sizeof(vertex);
    descriptor.dependencyCount = 0;
    descriptor.chunkChecksum = Fnv1a64(destination.subspan(payloadOffset, sizeof(vertex)));

    // The caller's lie: e.g. an oversized byteSize, or a bogus
    // vertexLayoutId. Applied after honest defaults, before the section
    // checksum below is computed over whatever ends up actually written.
    corrupt(descriptor, sectionLength);

    std::memcpy(destination.data() + kSectionHeaderSize, &descriptor, sizeof(descriptor));

    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = generationId;
    header.sectionLength = sectionLength;
    header.chunkCount = 1;
    header.reserved = 0;
    header.sectionChecksum
        = Fnv1a64(destination.subspan(kSectionHeaderSize, sectionLength - kSectionHeaderSize));

    std::memcpy(destination.data(), &header, sizeof(header));

    return sectionLength;
}

} // namespace

int RunMutateAfterReady()
{
    auto requestAndSection = ReadRequestAndMapSection();
    if (!requestAndSection) {
        return 1;
    }
    auto& [request, view] = *requestAndSection;

    auto result = import_worker::GenerateSyntheticScene(view.bytes(), request.generationId,
                                                          request.sceneVariant, request.maxChunkCount);
    const auto* info = std::get_if<import_worker::GeneratedSectionInfo>(&result);
    if (info == nullptr) {
        return 1;
    }

    if (!SendChunksReady(request.generationId, info->chunkCount, info->sectionBytesWritten)) {
        return 1;
    }

    // Deliberately single-threaded: the corruption below happens strictly
    // before this function (and therefore main()) returns, on the same
    // thread -- so the process's exit is a genuine Win32 happens-before
    // guarantee that the corruption has already occurred. A detached
    // background thread would not be ordered against process exit and
    // would reintroduce exactly the kind of race this test needs to avoid.
    Sleep(kCorruptionDelayMs);

    // Corrupt vertex 0's px, the first 4 bytes of chunk 0's payload, at the
    // fixed offset SyntheticSceneGenerator.cpp itself computes as
    // chunk0Offset for a 2-chunk scene.
    uint64_t chunk0Offset = model_core::kSectionHeaderSize + 2 * model_core::kChunkDescriptorSize;
    float corruptedPx = -999.0f;
    std::memcpy(view.bytes().data() + chunk0Offset, &corruptedPx, sizeof(corruptedPx));

    return 0;
}

int RunReplayGeneration()
{
    auto requestAndSection = ReadRequestAndMapSection();
    if (!requestAndSection) {
        return 1;
    }
    auto& [request, view] = *requestAndSection;

    // Lie to the generator itself about which generation this is for --
    // every checksum ends up internally consistent (checksums don't cover
    // the generationId field), but the stamped ID is wrong relative to what
    // the host asked for.
    uint64_t staleGenerationId = request.generationId + 999;
    auto result = import_worker::GenerateSyntheticScene(view.bytes(), staleGenerationId,
                                                          request.sceneVariant, request.maxChunkCount);
    const auto* info = std::get_if<import_worker::GeneratedSectionInfo>(&result);
    if (info == nullptr) {
        return 1;
    }

    // The outer notice correctly claims the requested generation is done --
    // only the section's own stamped ID lies.
    if (!SendChunksReady(request.generationId, info->chunkCount, info->sectionBytesWritten)) {
        return 1;
    }
    return 0;
}

int RunLieOffset()
{
    auto requestAndSection = ReadRequestAndMapSection();
    if (!requestAndSection) {
        return 1;
    }
    auto& [request, view] = *requestAndSection;

    uint64_t sectionLength = BuildLyingSingleChunkSection(
        view.bytes(), request.generationId,
        [](model_core::ChunkDescriptor& descriptor, uint64_t honestSectionLength) {
            // Claim a payload range that overflows well past sectionLength.
            descriptor.byteSize = honestSectionLength + 1000;
            descriptor.normalizedRangeLength = descriptor.byteSize;
        });

    if (!SendChunksReady(request.generationId, 1, sectionLength)) {
        return 1;
    }
    return 0;
}

int RunLieLayout()
{
    auto requestAndSection = ReadRequestAndMapSection();
    if (!requestAndSection) {
        return 1;
    }
    auto& [request, view] = *requestAndSection;

    uint64_t sectionLength = BuildLyingSingleChunkSection(
        view.bytes(), request.generationId,
        [](model_core::ChunkDescriptor& descriptor, uint64_t /*honestSectionLength*/) {
            // Outside the closed VertexLayoutId enumeration.
            descriptor.vertexLayoutId = 9999;
        });

    if (!SendChunksReady(request.generationId, 1, sectionLength)) {
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Progressive-delivery attacks. See AttackModes.h for what each one proves.
// ---------------------------------------------------------------------------

namespace {

// The output-section fields are at the same offsets in every
// ParseXxxFileRequest -- they are field-for-field identical structs by this
// repo's "duplication over cross-format coupling" rule -- so one struct reads
// whichever of them RunImportSession chose to send. The source file handle is
// deliberately ignored: these modes fabricate output, and output is the only
// thing a host ever trusts a worker for.
struct BatchSessionRequest {
    uint64_t generationId = 0;
    uint64_t sectionHandleValue = 0;
    uint64_t sectionByteCapacity = 0;
    uint32_t maxChunkCount = 0;
};

bool IsFileImportOpcode(uint32_t opcode)
{
    return opcode == static_cast<uint32_t>(model_core::ControlOpcode::StartGltfImportFromFile)
        || opcode == static_cast<uint32_t>(model_core::ControlOpcode::StartStlImportFromFile)
        || opcode == static_cast<uint32_t>(model_core::ControlOpcode::StartPlyImportFromFile);
}

struct BatchSession {
    BatchSessionRequest request;
    platform::MappedView view;
};

std::optional<BatchSession> ReadFileImportRequestAndMapSection()
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (stdIn == nullptr || stdIn == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    auto received = model_core::ReadControlMessage(stdIn);
    if (!received || !IsFileImportOpcode(received->header.opcode)
        || received->payload.size() != sizeof(model_core::ParseGltfFileRequest)) {
        return std::nullopt;
    }

    model_core::ParseGltfFileRequest raw{};
    std::memcpy(&raw, received->payload.data(), sizeof(raw));

    BatchSession session;
    session.request.generationId = raw.generationId;
    session.request.sectionHandleValue = raw.sectionHandleValue;
    session.request.sectionByteCapacity = raw.sectionByteCapacity;
    session.request.maxChunkCount = raw.maxChunkCount;

    HANDLE section = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(raw.sectionHandleValue));
    session.view = platform::MappedView::Map(section, FILE_MAP_WRITE | FILE_MAP_READ,
                                              static_cast<SIZE_T>(raw.sectionByteCapacity));
    if (!session.view) {
        return std::nullopt;
    }
    return session;
}

// Writes one honest single-PointList-chunk section carrying `chunkId` and a
// vertex whose x is `marker`, so a test can tell which batch a chunk came
// from by looking at the geometry it accepted.
uint64_t BuildOneChunkSection(std::span<std::byte> destination, uint64_t generationId,
                               uint32_t chunkId, float marker)
{
    using namespace model_core;

    VertexPositionOnlyF32 vertex{ marker, 0.0f, 0.0f };

    uint64_t payloadOffset = kSectionHeaderSize + kChunkDescriptorSize;
    uint64_t sectionLength = payloadOffset + sizeof(vertex);

    std::memcpy(destination.data() + payloadOffset, &vertex, sizeof(vertex));

    ChunkDescriptor descriptor{};
    descriptor.normalizedRangeOffset = payloadOffset;
    descriptor.normalizedRangeLength = sizeof(vertex);
    descriptor.topology = ChunkTopology::PointList;
    descriptor.indexCount = 0;
    descriptor.vertexCount = 1;
    descriptor.vertexLayoutId = static_cast<uint32_t>(VertexLayoutId::PositionOnly_F32);
    descriptor.lodLevel = 0;
    descriptor.chunkId = chunkId;
    descriptor.byteSize = sizeof(vertex);
    descriptor.dependencyCount = 0;
    descriptor.chunkChecksum = Fnv1a64(destination.subspan(payloadOffset, sizeof(vertex)));

    std::memcpy(destination.data() + kSectionHeaderSize, &descriptor, sizeof(descriptor));

    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = generationId;
    header.sectionLength = sectionLength;
    header.chunkCount = 1;
    header.reserved = 0;
    header.sectionChecksum
        = Fnv1a64(destination.subspan(kSectionHeaderSize, sectionLength - kSectionHeaderSize));

    std::memcpy(destination.data(), &header, sizeof(header));
    return sectionLength;
}

bool SendBatchReady(uint64_t generationId, uint32_t batchIndex, uint32_t chunkCount,
                     uint64_t sectionBytesWritten)
{
    HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdOut == nullptr || stdOut == INVALID_HANDLE_VALUE) {
        return false;
    }
    model_core::ChunkBatchReadyNotice notice{};
    notice.generationId = generationId;
    notice.batchIndex = batchIndex;
    notice.chunkCount = chunkCount;
    notice.sectionBytesWritten = sectionBytesWritten;
    return model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::ChunkBatchReady, &notice,
                                            sizeof(notice));
}

// Blocks for the host's ack, as an honest worker must before touching the
// window again. Returns false if the host said anything else, or nothing.
bool AwaitBatchConsumed()
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (stdIn == nullptr || stdIn == INVALID_HANDLE_VALUE) {
        return false;
    }
    auto received = model_core::ReadControlMessage(stdIn);
    return received
        && received->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::ChunkBatchConsumed)
        && received->payload.size() == sizeof(model_core::ChunkBatchConsumedNotice);
}

// One honest batch: write it, announce it, wait to be told the window is free.
bool SendOneHonestBatch(std::span<std::byte> destination, uint64_t generationId, uint32_t batchIndex,
                         uint32_t chunkId, float marker)
{
    uint64_t length = BuildOneChunkSection(destination, generationId, chunkId, marker);
    return SendBatchReady(generationId, batchIndex, 1, length) && AwaitBatchConsumed();
}

} // namespace

int RunHonestBatches()
{
    auto session = ReadFileImportRequestAndMapSection();
    if (!session) {
        return 1;
    }
    auto& [request, view] = *session;

    if (!SendOneHonestBatch(view.bytes(), request.generationId, 0, 1, 10.0f)) {
        return 1;
    }
    if (!SendOneHonestBatch(view.bytes(), request.generationId, 1, 2, 20.0f)) {
        return 1;
    }

    uint64_t length = BuildOneChunkSection(view.bytes(), request.generationId, 3, 30.0f);
    return SendChunksReady(request.generationId, 1, length) ? 0 : 1;
}

int RunReplayBatchIndex()
{
    auto session = ReadFileImportRequestAndMapSection();
    if (!session) {
        return 1;
    }
    auto& [request, view] = *session;

    if (!SendOneHonestBatch(view.bytes(), request.generationId, 0, 1, 10.0f)) {
        return 1;
    }
    // Batch 0 again -- a fresh chunkId, so only the replayed index is wrong
    // and the rejection cannot be attributed to id reuse instead.
    uint64_t length = BuildOneChunkSection(view.bytes(), request.generationId, 2, 20.0f);
    SendBatchReady(request.generationId, 0, 1, length);
    return 0;
}

int RunSkipBatchIndex()
{
    auto session = ReadFileImportRequestAndMapSection();
    if (!session) {
        return 1;
    }
    auto& [request, view] = *session;

    if (!SendOneHonestBatch(view.bytes(), request.generationId, 0, 1, 10.0f)) {
        return 1;
    }
    uint64_t length = BuildOneChunkSection(view.bytes(), request.generationId, 2, 20.0f);
    SendBatchReady(request.generationId, 2, 1, length); // 1 is missing
    return 0;
}

int RunReuseChunkIdAcrossBatches()
{
    auto session = ReadFileImportRequestAndMapSection();
    if (!session) {
        return 1;
    }
    auto& [request, view] = *session;

    if (!SendOneHonestBatch(view.bytes(), request.generationId, 0, 1, 10.0f)) {
        return 1;
    }
    // Correct index, different payload, but chunkId 1 all over again.
    uint64_t length = BuildOneChunkSection(view.bytes(), request.generationId, 1, 99.0f);
    SendBatchReady(request.generationId, 1, 1, length);
    return 0;
}

int RunUnboundedBatches()
{
    auto session = ReadFileImportRequestAndMapSection();
    if (!session) {
        return 1;
    }
    auto& [request, view] = *session;

    // Every batch individually honest; only the absence of an end is the
    // attack. The host is expected to stop this at its cap -- and then the
    // Job Object kill-on-close to stop the process itself, which is why this
    // loop never needs its own exit.
    for (uint32_t batchIndex = 0;; ++batchIndex) {
        if (!SendOneHonestBatch(view.bytes(), request.generationId, batchIndex, batchIndex + 1,
                                 static_cast<float>(batchIndex))) {
            return 1;
        }
    }
}

int RunWriteBeforeAck()
{
    auto session = ReadFileImportRequestAndMapSection();
    if (!session) {
        return 1;
    }
    auto& [request, view] = *session;

    // Announce batch 0, then immediately scribble over the window without
    // waiting to be told the host is done with it. Whatever the host accepted
    // for batch 0 must be what it copied, not what is there now.
    uint64_t length = BuildOneChunkSection(view.bytes(), request.generationId, 1, 10.0f);
    if (!SendBatchReady(request.generationId, 0, 1, length)) {
        return 1;
    }
    for (int i = 0; i < 200; ++i) {
        BuildOneChunkSection(view.bytes(), request.generationId, 1, -999.0f);
    }
    if (!AwaitBatchConsumed()) {
        return 1;
    }

    uint64_t finalLength = BuildOneChunkSection(view.bytes(), request.generationId, 2, 20.0f);
    return SendChunksReady(request.generationId, 1, finalLength) ? 0 : 1;
}

int RunBatchAfterTerminal()
{
    auto session = ReadFileImportRequestAndMapSection();
    if (!session) {
        return 1;
    }
    auto& [request, view] = *session;

    uint64_t length = BuildOneChunkSection(view.bytes(), request.generationId, 1, 10.0f);
    if (!SendChunksReady(request.generationId, 1, length)) {
        return 1;
    }
    // Past the end of the generation: the host must already be done reading.
    uint64_t extra = BuildOneChunkSection(view.bytes(), request.generationId, 2, 20.0f);
    SendBatchReady(request.generationId, 1, 1, extra);
    return 0;
}

} // namespace hostile_worker

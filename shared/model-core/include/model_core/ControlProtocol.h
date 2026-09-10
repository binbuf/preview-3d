#pragma once

#include "model_core/ImportError.h"

#include <cstdint>

namespace model_core {

enum class ControlOpcode : uint32_t {
    StartGeneration = 1,          // host -> worker
    ChunksReady = 2,              // worker -> host
    GenerationError = 3,          // worker -> host
    StartGltfImport = 4,          // host -> worker
    StartGltfImportFromFile = 5,  // host -> worker
    Shutdown = 6,                 // host -> worker, --pool mode only: exit cleanly, no reply
    StartStlImportFromFile = 7,   // host -> worker
    StartPlyImportFromFile = 8,   // host -> worker
    // Mid-generation, worker -> host, zero or more times before the
    // terminal ChunksReady/GenerationError reply -- a genuine deviation
    // from every opcode above's strict one-shot-per-generation shape. The
    // worker's own (already-sandboxed) glTF parse is what discovers a
    // .gltf's external sidecar references; only the trusted host may ever
    // open a path, so the worker must ask for one by relative reference.
    // See import_broker/SidecarRequestServicer.h (host) and
    // import-worker/src/SidecarFileClient.h (worker).
    RequestSidecarFile = 9,       // worker -> host
    SidecarFileReady = 10,        // host -> worker
    SidecarFileUnavailable = 11,  // host -> worker
    // Non-terminal progressive delivery. A model larger than the output
    // window cannot cross it in one write, so a generation may now fill and
    // hand over the section repeatedly: zero or more
    // ChunkBatchReady/ChunkBatchConsumed round trips, then the terminal
    // ChunksReady carrying the final batch.
    //
    // ChunksReady keeps its exact existing meaning ("the section holds a
    // batch; validate it") and layout, so a model that fits in one window
    // sends no ChunkBatchReady at all and its byte traffic is identical to
    // before this opcode existed -- the same "leave the already-tested path
    // untouched" rule every opcode above followed.
    //
    // ChunkBatchConsumed is flow control, not an acknowledgement of content:
    // the section is a *reused* window, so without it a worker would race
    // ahead and overwrite bytes the host is still copying out. The worker
    // must block for it before touching the section again.
    ChunkBatchReady = 12,         // worker -> host, non-terminal
    ChunkBatchConsumed = 13,      // host -> worker
};

// Bounded so a corrupt/oversized declared payload size can never drive an
// unbounded allocation or read in ControlChannelIo::ReadControlMessage.
constexpr uint32_t kMaxControlPayloadBytes = 256;

#pragma pack(push, 1)

struct ControlMessageHeader {
    uint32_t opcode;      // ControlOpcode value
    uint32_t payloadSize; // exact byte size of the payload that follows; must be <= kMaxControlPayloadBytes
};
static_assert(sizeof(ControlMessageHeader) == 8, "ControlMessageHeader layout changed");

enum : uint32_t {
    kSceneVariant_CubeAndPointCluster = 1,
};

struct StartGenerationRequest {
    uint64_t generationId;
    uint32_t sceneVariant;       // selects which synthetic scene the worker fabricates
    uint32_t reserved0;
    uint64_t sectionHandleValue; // the inherited shared-section HANDLE's numeric value, as an
                                   // opaque fixed-width integer (never a raw HANDLE/void* in the
                                   // struct, for explicit/portable layout) -- valid because
                                   // Windows handle inheritance preserves the numeric value
    uint64_t sectionByteCapacity; // how many bytes of the section the worker may use
    uint32_t maxChunkCount;       // sanity cap on chunk count the worker may emit
    uint32_t reserved1;
};
static_assert(sizeof(StartGenerationRequest) == 40, "StartGenerationRequest layout changed");

struct ChunksReadyNotice {
    uint64_t generationId;
    uint32_t chunkCount;
    uint32_t reserved0;
    uint64_t sectionBytesWritten; // diagnostic only -- the validator always re-derives and
                                    // bounds-checks the authoritative length from
                                    // SectionHeader::sectionLength itself, never trusts this
                                    // separate claim
};
static_assert(sizeof(ChunksReadyNotice) == 24, "ChunksReadyNotice layout changed");

// Real-parser request: an input GLB source section plus the same output
// section fields StartGenerationRequest carries. Kept as a distinct
// request/opcode rather than a modification of StartGenerationRequest, so
// the synthetic generator's already-tested request/reply path stays
// byte-for-byte untouched. Replies reuse ChunksReadyNotice/
// GenerationErrorNotice unmodified -- the worker's reply shape doesn't need
// to differ by request type.
struct ParseGltfRequest {
    uint64_t generationId;
    uint64_t sourceHandleValue;   // inherited INPUT-section HANDLE, numeric value (same convention
                                    // as StartGenerationRequest::sectionHandleValue)
    uint64_t sourceByteLength;    // exact GLB byte length within that section
    uint64_t sectionHandleValue;  // inherited OUTPUT-section HANDLE, numeric value
    uint64_t sectionByteCapacity; // output section capacity
    uint32_t maxChunkCount;       // sanity cap on chunk count the worker may emit
    uint32_t reserved0;
};
static_assert(sizeof(ParseGltfRequest) == 48, "ParseGltfRequest layout changed");

// Real-file request: the trusted process opened and canonicalized a real
// on-disk source (import_broker::OpenAndCanonicalizeSourceFile) and the
// broker duplicated its raw FILE handle -- not a pre-made file-mapping/
// pagefile-section handle -- into the worker's inherited handle set. The
// worker builds its own model_core::MappedFile from that handle
// (MappedFile::FromHandle) per .docs/design/03-file-formats-and-ingestion.md's
// "Mapped-file abstraction" step 1. Kept as a distinct request/opcode
// rather than a modification of ParseGltfRequest/StartGltfImport, same
// additive precedent that request already set relative to
// StartGenerationRequest -- the shared-section synthetic/real-glTF paths
// stay byte-for-byte untouched. Replies reuse ChunksReadyNotice/
// GenerationErrorNotice unmodified.
struct ParseGltfFileRequest {
    uint64_t generationId;
    uint64_t sourceFileHandleValue; // inherited raw FILE handle (not a mapping/section handle),
                                      // numeric value -- the worker maps it itself
    uint64_t sectionHandleValue;    // inherited OUTPUT-section HANDLE, numeric value
    uint64_t sectionByteCapacity;   // output section capacity
    uint32_t maxChunkCount;         // sanity cap on chunk count the worker may emit
    uint32_t reserved0;
};
static_assert(sizeof(ParseGltfFileRequest) == 40, "ParseGltfFileRequest layout changed");

// Real-file request for the binary-STL adapter (StlAdapter.cpp) -- same
// shape as ParseGltfFileRequest (a raw duplicated FILE handle plus output-
// section fields), but its own named struct rather than reusing
// ParseGltfFileRequest for a different format, matching this repo's
// established "small deliberate duplication over cross-format coupling"
// precedent (ParseGltfRequest itself didn't reuse StartGenerationRequest's
// similar shape either). Replies reuse ChunksReadyNotice/
// GenerationErrorNotice unmodified.
struct ParseStlFileRequest {
    uint64_t generationId;
    uint64_t sourceFileHandleValue; // inherited raw FILE handle (not a mapping/section handle),
                                      // numeric value -- the worker maps it itself
    uint64_t sectionHandleValue;    // inherited OUTPUT-section HANDLE, numeric value
    uint64_t sectionByteCapacity;   // output section capacity
    uint32_t maxChunkCount;         // sanity cap on chunk count the worker may emit
    uint32_t reserved0;
};
static_assert(sizeof(ParseStlFileRequest) == 40, "ParseStlFileRequest layout changed");

// Real-file request for the binary-PLY adapter (PlyAdapter.cpp) -- same
// shape as ParseStlFileRequest/ParseGltfFileRequest (a raw duplicated FILE
// handle plus output-section fields), but its own named struct rather than
// reusing either for a different format, matching this repo's established
// "small deliberate duplication over cross-format coupling" precedent.
// Replies reuse ChunksReadyNotice/GenerationErrorNotice unmodified.
struct ParsePlyFileRequest {
    uint64_t generationId;
    uint64_t sourceFileHandleValue; // inherited raw FILE handle (not a mapping/section handle),
                                      // numeric value -- the worker maps it itself
    uint64_t sectionHandleValue;    // inherited OUTPUT-section HANDLE, numeric value
    uint64_t sectionByteCapacity;   // output section capacity
    uint32_t maxChunkCount;         // sanity cap on chunk count the worker may emit
    uint32_t reserved0;
};
static_assert(sizeof(ParsePlyFileRequest) == 40, "ParsePlyFileRequest layout changed");

struct GenerationErrorNotice {
    uint64_t generationId;
    uint32_t errorCode; // ImportErrorCode value
    uint32_t reserved0;
};
static_assert(sizeof(GenerationErrorNotice) == 16, "GenerationErrorNotice layout changed");

// UTF-8, glTF-URI-decoded, NOT NUL-terminated -- relativePathLength is the
// exact meaningful prefix of relativePathUtf8. No sequence number: the
// control channel stays strictly synchronous (the worker blocks for one
// reply before sending its next request), so a seq field would be dead
// weight for a pipelining case that doesn't exist.
constexpr uint32_t kMaxSidecarRelativePathBytes = 200;

struct RequestSidecarFileNotice {
    uint64_t generationId;
    uint32_t relativePathLength; // <= kMaxSidecarRelativePathBytes
    uint32_t reserved0;
    uint8_t relativePathUtf8[kMaxSidecarRelativePathBytes];
};
static_assert(sizeof(RequestSidecarFileNotice) == 216, "RequestSidecarFileNotice layout changed");

struct SidecarFileReadyNotice {
    uint64_t generationId;
    uint64_t sidecarFileHandleValue; // valid only in the WORKER's own handle table -- the host
                                       // DuplicateHandle'd it there directly (target-process
                                       // duplication, not launch-time PROC_THREAD_ATTRIBUTE_HANDLE_LIST
                                       // inheritance, since the worker is already running)
    uint64_t sidecarByteLength;       // host-measured via GetFileSizeEx; the worker must not trust
                                       // any other size claim for this file
    uint32_t reserved0;
    uint32_t reserved1;
};
static_assert(sizeof(SidecarFileReadyNotice) == 32, "SidecarFileReadyNotice layout changed");

struct SidecarFileUnavailableNotice {
    uint64_t generationId;
    uint32_t errorCode; // ImportErrorCode: UnsafeReference or FileUnavailable
    uint32_t reserved0;
};
static_assert(sizeof(SidecarFileUnavailableNotice) == 16, "SidecarFileUnavailableNotice layout changed");

// One non-terminal batch is ready in the output section. Deliberately
// field-for-field ChunksReadyNotice plus a batchIndex rather than a reuse of
// it: the host must be able to tell a replayed or skipped batch from an
// in-order one, and ChunksReadyNotice has no field free to carry that.
//
// batchIndex is 0-based and must increase by exactly one per batch within a
// generation. Like sectionBytesWritten it is a *claim*, not a fact -- the
// host tracks its own expected index and compares, exactly as the validator
// re-derives sectionLength rather than trusting this struct's byte count.
struct ChunkBatchReadyNotice {
    uint64_t generationId;
    uint32_t batchIndex;
    uint32_t chunkCount;
    uint64_t sectionBytesWritten; // diagnostic only -- see ChunksReadyNotice
};
static_assert(sizeof(ChunkBatchReadyNotice) == 24, "ChunkBatchReadyNotice layout changed");

// The host has finished copying that batch out of the section; the window is
// free for reuse. Echoes batchIndex so a worker cannot mistake an ack for an
// earlier batch as permission to overwrite a later one.
struct ChunkBatchConsumedNotice {
    uint64_t generationId;
    uint32_t batchIndex;
    uint32_t reserved0;
};
static_assert(sizeof(ChunkBatchConsumedNotice) == 16, "ChunkBatchConsumedNotice layout changed");

#pragma pack(pop)

} // namespace model_core

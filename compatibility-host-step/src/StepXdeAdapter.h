#pragma once

// STEP-003/004 production XDE scene adapter. It runs behind the STEP-002
// `StartStepImportFromFile` route after `StepPart21Preflight` has accepted the
// ISO 10303-21 physical file, reads the accepted bytes exclusively through a
// product-owned seekable stream over the inherited read-only source handle,
// transfers them with the pinned OCCT XDE reader, and emits only the existing
// normalized protocol-v10 node / reusable-geometry / mesh-instance / material
// contract.
//
// STEP-004 adds the versioned tessellation-quality profile, per-definition
// bounded extraction with cluster-local positions and double origins, and
// progressive multi-window delivery: geometry that does not fit one output
// window is handed off through the existing ChunkBatchReady/ChunkBatchConsumed
// protocol instead of requiring the whole normalized scene in memory at once.
//
// Design authority: .docs/stp.md (STEP-003, STEP-004). The trusted viewer and
// the general import worker never link or load this adapter or its OCCT
// closure.

#include "model_core/ControlProtocol.h"
#include "model_core/ImportError.h"

#include "StepTessellationProfile.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <span>

namespace step_host {

// Bounded, documented STEP scene ceilings. They are admission/emission bounds,
// not a promise that an OCCT translation of every limit-sized file fits; the
// broker's Tier-B source/triangle/vertex/object/material caps remain
// independently enforced by SharedSectionValidator.
struct StepXdeLimits {
    std::uint32_t maxNodes = 50'000;        // Tier-B object limit
    std::uint32_t maxDefinitions = 50'000;  // reusable geometry definitions
    std::uint32_t maxMaterials = 32'768;    // Tier-B material limit
    std::uint32_t maxSubshapes = 1'000'000; // bounded face/subshape walk
    std::uint32_t maxHierarchyDepth = 256;  // protocol scene hierarchy depth
    std::uint32_t maxTriangles = 20'000'000; // Tier-B triangle limit
    std::uint32_t maxVertices = 60'000'000;  // Tier-B vertex limit
    StepTessellationProfile profile{};
};

// Hands one complete non-terminal output window to the host control channel.
// Returns false when the batch could not be accepted (host gone, cancellation,
// or an out-of-order refusal), in which case the adapter abandons the
// generation rather than writing into the window again. A default-constructed
// publisher keeps the pre-STEP-004 single-window behavior: a scene that does
// not fit the section fails as `ResourceLimit`.
using StepBatchPublisher =
    std::function<bool(std::uint32_t chunkCount, std::uint64_t sectionBytesWritten)>;

// STEP-005 bounded phase/progress event. `phase` is one of the closed
// model_core::kStepPhase* identities; every other field is a product-owned
// counter. The host turns one of these into a control-channel StepProgress
// message, so no OCCT string, label, or path can cross the boundary.
struct StepProgressEvent {
    std::uint32_t phase = 0;
    std::uint32_t definitionsMeshed = 0;
    std::uint32_t definitionTotal = 0;
    std::uint64_t preflightBytes = 0;
    std::uint64_t phaseMilliseconds = 0;
    std::uint64_t totalMilliseconds = 0;
};
using StepProgressSink = std::function<void(const StepProgressEvent&)>;

// STEP-005 where-the-time-goes evidence. Each value is wall-clock elapsed
// milliseconds for one pipeline phase; `totalMilliseconds` covers the whole
// adapter call. They are recorded on the result (and published through
// StepProgress) so STEP-005/STEP-008 can report the parse/transfer/mesh split
// instead of assuming the advisor's model applies to this constrained build.
struct StepPhaseTimings {
    std::uint64_t preflightMilliseconds = 0;
    std::uint64_t readMilliseconds = 0;
    std::uint64_t transferMilliseconds = 0;
    std::uint64_t planMilliseconds = 0;
    std::uint64_t meshMilliseconds = 0;
    std::uint64_t extractMilliseconds = 0;
    std::uint64_t emitMilliseconds = 0;
    std::uint64_t totalMilliseconds = 0;
};

struct StepXdeResult {
    model_core::ImportErrorCode errorCode = model_core::ImportErrorCode::None;
    std::uint32_t chunkCount = 0;        // terminal window only
    std::uint32_t batchCount = 0;        // non-terminal windows already handed off
    std::uint64_t sectionBytesWritten = 0;
    std::uint32_t definitionCount = 0;
    std::uint32_t nodeCount = 0;
    std::uint32_t instanceCount = 0;
    std::uint32_t materialCount = 0;
    std::uint32_t warningCount = 0;
    // STEP-004 work item 7 / STEP-005 work item 2: per-phase cost evidence.
    StepPhaseTimings timings{};
};

// Runs the accepted XDE traversal into `section`. `section` must be exactly
// `request.sectionByteCapacity` bytes. On success the section already carries a
// self-consistent protocol-v10 header, descriptor table, and payloads ready
// for the broker's copy-then-validate acceptance rule. OCCT/label/path strings
// never cross this boundary.
//
// STEP-005: when `mappedSource` is non-empty the reader consumes a product-owned
// stream over that already-mapped read-only view of the inherited handle
// (single read for preflight plus transfer); otherwise it falls back to the
// proven `HandleStreamBuf` over `sourceHandle`. `progress` receives bounded
// phase events; it is optional.
StepXdeResult RunStepXdeAdapter(const model_core::ParseStepFileRequest& request,
                                std::span<std::byte> section, HANDLE sourceHandle,
                                HANDLE cancellationEvent,
                                std::span<const std::byte> mappedSource = {},
                                const StepXdeLimits& limits = {},
                                const StepBatchPublisher& publish = {},
                                const StepProgressSink& progress = {});

} // namespace step_host
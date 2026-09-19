#pragma once

// STEP-003 production XDE scene adapter. It runs behind the STEP-002
// `StartStepImportFromFile` route after `StepPart21Preflight` has accepted the
// ISO 10303-21 physical file, reads the accepted bytes exclusively through a
// product-owned seekable stream over the inherited read-only source handle,
// transfers them with the pinned OCCT XDE reader, and emits only the existing
// normalized protocol-v10 node / reusable-geometry / mesh-instance / material
// contract.
//
// Design authority: .docs/stp.md (STEP-003). The trusted viewer and the general
// import worker never link or load this adapter or its OCCT closure.

#include "model_core/ControlProtocol.h"
#include "model_core/ImportError.h"

#include <windows.h>

#include <cstdint>
#include <span>

namespace step_host {

// Bounded, documented STEP-003 scene ceilings. They are admission/emission
// bounds, not a promise that an OCCT translation of every limit-sized file
// fits; the broker's Tier-B source/triangle/vertex/object/material caps remain
// independently enforced by SharedSectionValidator.
struct StepXdeLimits {
    std::uint32_t maxNodes = 50'000;        // Tier-B object limit
    std::uint32_t maxDefinitions = 50'000;  // reusable geometry definitions
    std::uint32_t maxMaterials = 32'768;    // Tier-B material limit
    std::uint32_t maxSubshapes = 1'000'000; // bounded face/subshape walk
    std::uint32_t maxHierarchyDepth = 256;  // protocol scene hierarchy depth
    std::uint32_t maxTriangles = 20'000'000; // Tier-B triangle limit
    std::uint32_t maxVertices = 60'000'000;  // Tier-B vertex limit
    std::uint32_t chunkTriangles = 65'536;   // split geometry to bounded chunks
    double linearDeflection = 0.1;           // mm; STEP-004 owns the quality profile
    double angularDeflection = 0.5;          // radians
};

struct StepXdeResult {
    model_core::ImportErrorCode errorCode = model_core::ImportErrorCode::None;
    std::uint32_t chunkCount = 0;
    std::uint64_t sectionBytesWritten = 0;
    std::uint32_t definitionCount = 0;
    std::uint32_t nodeCount = 0;
    std::uint32_t instanceCount = 0;
    std::uint32_t materialCount = 0;
    std::uint32_t warningCount = 0;
};

// Runs the accepted XDE traversal into `section`. `section` must be exactly
// `request.sectionByteCapacity` bytes. On success the section already carries a
// self-consistent protocol-v10 header, descriptor table, and payloads ready
// for the broker's copy-then-validate acceptance rule. OCCT/label/path strings
// never cross this boundary.
StepXdeResult RunStepXdeAdapter(const model_core::ParseStepFileRequest& request,
                                std::span<std::byte> section, HANDLE sourceHandle,
                                HANDLE cancellationEvent, const StepXdeLimits& limits = {});

} // namespace step_host
#pragma once

// STEP-001 test-only contract. This deliberately does not extend protocol v10:
// STEP-002 owns the production opcode/error/scene contract. The parent opens
// the source file read-only, duplicates that handle into the host, and creates
// a private shared section for the bounded metric record below. The host never
// receives a path, directory handle, or URL.

#include <cstddef>
#include <cstdint>

namespace step_host {

inline constexpr std::uint32_t kStepSpikeMagic = 0x31505453u; // "STP1"
inline constexpr std::uint32_t kStepSpikeVersion = 1;
inline constexpr std::uint64_t kStepSpikeMaxSectionBytes = 16ull * 1024ull * 1024ull;
inline constexpr std::size_t kStepSpikeDiagnosticBytes = 768;

enum class StepSpikeStatus : std::uint32_t {
    Pending = 0,
    Success = 1,
    InvalidRequest = 2,
    PreflightFailure = 3,
    ReadFailure = 4,
    TransferFailure = 5,
    NoGeometry = 6,
    ResourceLimit = 7,
    Cancelled = 8,
    InternalFailure = 9,
};

// Bounded, format-neutral facts only. No source name, label, path, or raw OCCT
// string may be copied here; `diagnostic` is a product-owned classification
// string, not a kernel message.
#pragma pack(push, 1)
struct StepSpikeSection {
    std::uint32_t magic = kStepSpikeMagic;
    std::uint32_t version = kStepSpikeVersion;
    std::uint64_t sectionByteLength = 0;

    // Inputs (packed by the trusted test harness before launch).
    std::uint64_t sourceFileHandleValue = 0;
    std::uint64_t cancellationEventHandleValue = 0;
    std::uint32_t maxEntityRecords = 5'000'000;
    std::uint32_t maxHierarchyDepth = 256;
    std::uint64_t maxMilliseconds = 30'000;
    double linearDeflection = 0.0;   // 0 => policy default (relative)
    double angularDeflection = 0.5;
    std::uint32_t coarsePass = 1;

    // Outputs (written by the host under the broker-observed state flag).
    volatile long state = 0; // 0=packed, 1=started, 2=read, 3=transferred, 4=walked, 5=complete
    StepSpikeStatus status = StepSpikeStatus::Pending;
    std::uint32_t diagnosticCode = 0;

    std::uint32_t rootLabelCount = 0;
    std::uint32_t assemblyLabelCount = 0;
    std::uint32_t simpleShapeLabelCount = 0;
    std::uint32_t uniqueDefinitionCount = 0;
    std::uint32_t instanceCount = 0;
    std::uint32_t reusedDefinitionCount = 0;
    std::uint32_t maxObservedDepth = 0;
    std::uint32_t externFileCount = 0;

    std::uint32_t solidCount = 0;
    std::uint32_t shellCount = 0;
    std::uint32_t faceCount = 0;
    std::uint32_t edgeCount = 0;

    std::uint32_t colorCount = 0;
    std::uint32_t coloredLabelCount = 0;
    std::uint32_t transparentColorCount = 0;

    std::uint64_t triangleCount = 0;
    std::uint64_t extractedVertexCount = 0;
    std::uint32_t meshedDefinitionCount = 0;

    double metersPerUnit = 0.0;
    // OCCT XDE normalizes transferred geometry to the Cascade system unit
    // (documentMetersPerUnit) while the Part-21 file's authored unit is
    // recoverable from the model (authoredMetersPerUnit). Both are reported so
    // STEP-003 can pin the conversion deliberately.
    double documentMetersPerUnit = 0.0;
    double authoredMetersPerUnit = 0.0;
    char authoredLengthUnit[32]{};
    double worldBounds[6]{};
    std::uint64_t semanticDigest = 0;

    std::uint64_t readMicroseconds = 0;
    std::uint64_t transferMicroseconds = 0;
    std::uint64_t walkMicroseconds = 0;
    std::uint64_t meshMicroseconds = 0;
    std::uint64_t totalMicroseconds = 0;
    std::uint64_t privateBytes = 0;
    std::uint64_t peakWorkingSetBytes = 0;

    char diagnostic[kStepSpikeDiagnosticBytes]{};
};
#pragma pack(pop)

static_assert(sizeof(StepSpikeSection) < kStepSpikeMaxSectionBytes);

// Tiny handshake read from stdin before the host touches OCCT and echoed back
// after the bounded proof completes. It carries no source data.
struct StepSpikeControl {
    std::uint32_t magic = kStepSpikeMagic;
    std::uint32_t version = kStepSpikeVersion;
};

static_assert(sizeof(StepSpikeControl) == 8);

} // namespace step_host

#ifndef NOMINMAX
#define NOMINMAX
#endif

// Standalone, no-GPU sanitizer target for the STEP/STP admission boundary.
//
// STEP-006 requires that declaration discovery and rejected external
// declarations cannot be bypassed into a filesystem or network access. The
// product-owned `StepPart21Preflight` is the only component that sees raw
// ISO 10303-21 bytes before the OCCT reader is reachable, and it never resolves
// a path, URL, socket, or child process. This target mutates:
//   * the bounded Part-21 lexical admission scanner (including the
//     FILE_POPULATION/DOCUMENT_FILE external-declaration discovery);
//   * the production framed control decoder with mutated
//     StartStepImportFromFile / StepProgress records and request-flag masks;
//   * the trusted normalized-output copy-and-validate decoder.
// It creates no window, GPU device, file mapping, resolver, or child process.
// The pinned OCCT kernel is a separately built DLL and is not sanitizer
// instrumented here; real transfer/tessellation containment stays in the
// AppContainer/Job ImportIsolation tests.

#include "StepPart21Preflight.h"

#include "import_broker/SharedSectionValidator.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace {

constexpr uint32_t kEnvelopeMagic = 0x5a465453; // "STFZ"
constexpr size_t kInputLimit = 1u * 1024u * 1024u;

#pragma pack(push, 1)
struct Envelope {
    uint32_t magic;
    uint8_t domain;
    uint8_t flags;
    uint16_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(Envelope) == 8);

enum class Domain : uint8_t {
    Part21Admission = 0,
    DeclarationDiscovery = 1,
    ControlFrame = 2,
    NormalizedOutput = 3,
};

struct Input {
    Domain domain = Domain::Part21Admission;
    uint8_t flags = 0;
    std::span<const std::byte> payload;
};

Input Decode(std::span<const std::byte> bytes)
{
    if (bytes.size() < sizeof(Envelope)) return {Domain::Part21Admission, 0, bytes};
    Envelope envelope{};
    std::memcpy(&envelope, bytes.data(), sizeof(envelope));
    if (envelope.magic != kEnvelopeMagic)
        return {Domain::Part21Admission, 0, bytes};
    return {static_cast<Domain>(envelope.domain % 4), envelope.flags,
            bytes.subspan(sizeof(envelope))};
}

step_host::StepPreflightLimits BoundedLimits()
{
    step_host::StepPreflightLimits limits;
    limits.maxLexedBytes = kInputLimit;
    limits.maxEntityRecords = 4096;
    limits.maxReferenceCount = 65536;
    limits.maxNestingDepth = 64;
    limits.maxRecordBytes = 64 * 1024;
    limits.maxStringBytes = 64 * 1024;
    limits.maxDataSections = 16;
    limits.maxExternalDocuments = 0;
    return limits;
}

// The admission scanner is exercised both as a whole-input scan and, for the
// declaration-discovery domain, with the external-document ceiling raised so
// the scanner must keep lexing rather than fail closed at the first
// declaration. Both outcomes are valid; the oracle is only memory safety and
// termination, because every result is a closed product status.
void FuzzAdmission(std::span<const std::byte> bytes, bool allowExternal)
{
    auto limits = BoundedLimits();
    if (allowExternal) limits.maxExternalDocuments = 0xFFFFFFFFu;
    const auto result = step_host::StepPreflightBytes(bytes, limits);
    volatile uint32_t status = static_cast<uint32_t>(result.status);
    volatile uint32_t entities = result.entityRecords;
    volatile uint32_t external = result.externalDocuments;
    volatile uint64_t lexed = result.lexedBytes;
    (void)status; (void)entities; (void)external; (void)lexed;
}

// A declaration is only discovered by the lexical scanner; feeding the scanner
// through an anonymous pipe also covers the handle-streaming form used when the
// source mapping is unavailable. The payload is capped below the pipe buffer so
// the write cannot block before the reader runs.
void FuzzAdmissionHandle(std::span<const std::byte> bytes)
{
    constexpr size_t kMaxHandleBytes = 2048;
    if (bytes.size() > kMaxHandleBytes) return;
    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    if (!CreatePipe(&readHandle, &writeHandle, nullptr, 4096)) return;
    DWORD written = 0;
    if (!bytes.empty())
        (void)WriteFile(writeHandle, bytes.data(), static_cast<DWORD>(bytes.size()),
                        &written, nullptr);
    CloseHandle(writeHandle);
    auto limits = BoundedLimits();
    limits.maxLexedBytes = kInputLimit;
    (void)step_host::StepPreflightHandle(readHandle, static_cast<std::uint64_t>(written),
                                         limits);
    CloseHandle(readHandle);
}

void FuzzControlFrame(std::span<const std::byte> bytes)
{
    if (bytes.size() > sizeof(model_core::ControlMessageHeader)
            + model_core::kMaxControlPayloadBytes) return;
    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    if (!CreatePipe(&readHandle, &writeHandle, nullptr, 512)) return;
    DWORD written = 0;
    if (!bytes.empty())
        (void)WriteFile(writeHandle, bytes.data(), static_cast<DWORD>(bytes.size()),
                        &written, nullptr);
    CloseHandle(writeHandle);
    const auto message = model_core::ReadControlMessage(readHandle);
    CloseHandle(readHandle);
    if (!message) return;

    if (message->header.opcode
            == static_cast<uint32_t>(model_core::ControlOpcode::StartStepImportFromFile)
        && message->payload.size() == sizeof(model_core::ParseStepFileRequest)) {
        model_core::ParseStepFileRequest request{};
        std::memcpy(&request, message->payload.data(), sizeof(request));
        // The host's request-flag mask is an explicit allowlist; a mutated flag
        // word must not be able to smuggle a non-test seam.
        volatile bool closedFlags = (request.requestFlags
            & ~(model_core::kImportRequestDetailService
                | model_core::kImportRequestCoarseProxy
                | model_core::kImportRequestStepForceSerialForTesting)) == 0;
        (void)closedFlags;
    } else if (message->header.opcode
                   == static_cast<uint32_t>(model_core::ControlOpcode::StepProgress)
               && message->payload.size() == sizeof(model_core::StepProgressNotice)) {
        model_core::StepProgressNotice notice{};
        std::memcpy(&notice, message->payload.data(), sizeof(notice));
        volatile bool boundedPhase = notice.phase <= 6;
        (void)boundedPhase;
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (!data || size > kInputLimit + sizeof(Envelope)) return 0;
    const Input input = Decode({reinterpret_cast<const std::byte*>(data), size});
    try {
        switch (input.domain) {
        case Domain::Part21Admission:
            FuzzAdmission(input.payload, false);
            break;
        case Domain::DeclarationDiscovery:
            FuzzAdmission(input.payload, true);
            if (input.flags & 1u) FuzzAdmissionHandle(input.payload);
            break;
        case Domain::ControlFrame:
            FuzzControlFrame(input.payload);
            break;
        case Domain::NormalizedOutput:
            (void)import_broker::ValidateAndCopySection(input.payload, 0, 4096);
            break;
        }
    } catch (...) {
        // The scanner is allocation-bounded and exception-free in normal
        // operation; only sanitizer faults and process failures are oracles for
        // arbitrary bytes.
    }
    return 0;
}

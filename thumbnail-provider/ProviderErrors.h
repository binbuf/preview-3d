#pragma once

// T06 HRESULT mapping.
//
// design/05-thumbnail-provider.md ("HRESULT mapping") fixes the only codes the
// provider may return. `HresultFor` is the single mapping from the provider's
// COM-boundary outcome to those exact values; `ClassifyError` folds the
// adapter-facing `model_core::ImportErrorCode` taxonomy (design/03, "Error
// taxonomy") into one of those rows; `HresultForError` composes the two so the
// COM core (T11/T13) can translate an adapter result in one call.
//
// The tabulated values are used verbatim; no new code is invented. A fabricated
// success bitmap for a failed parse is prohibited (a failed call sets the
// output bitmap to null and returns the precise HRESULT).
//
// Mapping of the adapter taxonomy onto the table:
//   - BadArgument (E_INVALIDARG): a caller argument outside the COM contract
//     (the degenerate cx == 0 request), distinct from a bad pointer or an
//     out-of-order call (ADR-0015);
//   - Unsupported (ERROR_NOT_SUPPORTED): unsupported format/encoding/required
//     feature/composition, and unsafe references;
//   - BadFormat (ERROR_BAD_FORMAT): malformed/empty geometry and unusable or
//     changed input files;
//   - LimitExceeded (ERROR_FILE_TOO_LARGE): every resource/byte/count/archive
//     limit, including the per-component caps and host limits;
//   - Deadline (ERROR_TIMEOUT): the cooperative cancellation/timeout path;
//   - OutOfMemory (E_OUTOFMEMORY): allocation failure;
//   - DecoderFailure (E_FAIL, with a diagnostic event): importer/decoder faults
//     and internal failures.

#include "ProviderTypes.h"

#include <windows.h>

#include <cstdint>

namespace preview3d::provider {

// One row of the design/05 HRESULT table (plus success). Bad pointer, invalid
// call order and invalid argument are distinct because the table lists them
// separately; they are COM-boundary outcomes, not adapter-returned error codes.
enum class ProviderOutcome : std::uint32_t {
    Success = 0,
    BadPointer,          // E_POINTER
    InvalidCallSequence, // E_UNEXPECTED
    BadArgument,         // E_INVALIDARG (degenerate cx == 0; ADR-0015)
    Unsupported,         // HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)
    BadFormat,           // HRESULT_FROM_WIN32(ERROR_BAD_FORMAT)
    LimitExceeded,       // HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)
    Deadline,            // HRESULT_FROM_WIN32(ERROR_TIMEOUT)
    OutOfMemory,         // E_OUTOFMEMORY
    DecoderFailure,      // E_FAIL
};

// The single mapping from provider outcome to the tabulated HRESULT.
constexpr HRESULT HresultFor(ProviderOutcome outcome) noexcept
{
    using O = ProviderOutcome;
    switch (outcome) {
        case O::Success: return S_OK;
        case O::BadPointer: return E_POINTER;
        case O::InvalidCallSequence: return E_UNEXPECTED;
        case O::BadArgument: return E_INVALIDARG;
        case O::Unsupported: return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        case O::BadFormat: return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
        case O::LimitExceeded: return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        case O::Deadline: return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        case O::OutOfMemory: return E_OUTOFMEMORY;
        case O::DecoderFailure: return E_FAIL;
    }
    return E_FAIL;
}

// Folds an adapter-facing error code into the HRESULT table row it reports as.
constexpr ProviderOutcome ClassifyError(ErrorCode code) noexcept
{
    using O = ProviderOutcome;
    using E = model_core::ImportErrorCode;
    switch (code) {
        case E::None: return O::Success;
        case E::UnsupportedFormat:
        case E::UnsupportedEncoding:
        case E::UnsupportedRequiredFeature:
        case E::UnsupportedComposition:
        case E::UnsafeReference:
            return O::Unsupported;
        case E::MalformedData:
        case E::EmptyGeometry:
        case E::FileUnavailable:
        case E::FileChanged:
            return O::BadFormat;
        case E::ResourceLimit:
        case E::PrimarySourceLimit:
        case E::AggregateSourceLimit:
        case E::ScratchLimit:
        case E::ChunkCatalogLimit:
        case E::DracoPrimitiveLimit:
        case E::ArchiveLimit:
        case E::CompatibilityHostLimit:
        case E::StepHostLimit:
            return O::LimitExceeded;
        case E::Cancelled:
        case E::WorkerTimedOut:
            return O::Deadline;
        case E::OutOfMemory:
            return O::OutOfMemory;
        case E::InternalImporterFailure:
        case E::WorkerCrashed:
        case E::UploadFailure:
        case E::ImportProtocolViolation:
        case E::CompatibilityHostFailure:
        case E::StepHostFailure:
        case E::TessellationFailed:
            return O::DecoderFailure;
    }
    return O::DecoderFailure;
}

// Adapter result -> the exact HRESULT the COM boundary returns.
constexpr HRESULT HresultForError(ErrorCode code) noexcept
{
    return HresultFor(ClassifyError(code));
}

} // namespace preview3d::provider
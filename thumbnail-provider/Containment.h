#pragma once

// T16 last-resort exception and structured-exception containment.
//
// design/05-thumbnail-provider.md ("Security and robustness", "Threading and
// unload") and ADR-0005 place third-party parser calls behind exception and
// structured-exception containment at the COM boundary where legally safe,
// returning the tabulated HRESULT. Containment is a last-resort HRESULT
// boundary, never a correctness mechanism: an ordinary memory fault is still
// reported as a failure (never a fabricated success) and must be fixed by
// fuzzing/hardening (T43).
//
// The cooperative deadline is not an in-call interrupt. A third-party call
// already running on the Shell's calling thread is allowed to return before the
// request is rejected: `RunContained` measures the real elapsed time, records
// whether the 2 s stop point was passed, and only then fails an overrun to the
// generic icon. It never claims to have interrupted the call.
//
// This translation unit must be compiled with synchronous C++ exception
// handling (/EHsc) so a `catch (...) ` does not swallow a structured exception;
// the build sets that on Containment.cpp explicitly. It is PCH/COM-free so
// Tests.Unit.exe compiles the same source and injects exceptions/SEH directly.

#include "Deadline.h"
#include "Diagnostics.h"
#include "ProviderErrors.h"

#include <chrono>
#include <cstdint>

namespace preview3d::provider {

// A contained call. It returns a provider outcome and may throw a C++ exception
// or raise a structured exception from a third-party parser. It is intentionally
// NOT noexcept: a `noexcept` signature would call std::terminate before the
// boundary could translate the fault. Adapters whose frozen methods are
// noexcept must route their third-party calls through RunContained so nothing
// escapes the method.
using ContainedCall = ProviderOutcome (*)(void* context);

// What the boundary observed around one contained call.
struct ContainmentResult {
    ProviderOutcome outcome = ProviderOutcome::Success;
    bool cppException = false;
    bool structuredException = false;
    std::uint32_t structuredCode = 0;
    std::chrono::milliseconds elapsed{0};
    bool overranStop = false;
};

// Structured-exception codes this boundary refuses to swallow: a stack overflow
// cannot be handled safely in-process, and breakpoint/single-step belong to a
// debugger. Everything else (including an access violation) is translated to the
// tabulated failure so one hostile call cannot take down Explorer's surrogate.
bool ShouldContainStructuredCode(std::uint32_t code) noexcept;

// --- SEC-08 containment policy: quarantine on a contained structured fault ----
//
// A contained structured exception (an access violation, in-page error, illegal
// instruction, ...) is evidence the process may be running on corrupted state.
// The chosen policy is **quarantine**: the boundary records the fault, every
// later thumbnail request is refused with the tabulated failure before any
// parser runs, and requests already in flight are allowed to drain. A later
// request therefore never runs on suspected-corrupt state, and no success bitmap
// is fabricated. The state is process-global (the Shell surrogate is one
// process) and observable through `ContainmentQuarantined()`; the transition is
// reported through diagnostics as `DiagnosticStage::Containment` with
// `quarantined=true`. Stack overflow, breakpoint and single-step are not
// contained, so they never reach this state; a `__fastfail`/stack-cookie fault
// cannot be caught at all and kills the process (see design/05).
bool ContainmentQuarantined() noexcept;
std::uint32_t ContainmentQuarantineCode() noexcept;

// Records a contained structured fault. Idempotent (the first fault wins);
// called by the boundary and exposed so the policy is testable in-process.
void MarkContainmentQuarantined(std::uint32_t structuredCode) noexcept;

// Clears the quarantine. Test-only: a case that injects a fault must not leak
// the state into the next case in the same process. Never called by product code.
void ResetContainmentQuarantineForTest() noexcept;

// Runs `call(context)` under C++ exception and structured-exception containment.
// A C++ `std::bad_alloc` maps to E_OUTOFMEMORY; any other C++ exception and any
// contained structured exception map to E_FAIL (DecoderFailure). When `deadline`
// is non-null the real elapsed time is recorded and a completed call that passed
// the cooperative stop point is rejected with ERROR_TIMEOUT (Deadline). The
// observed outcome is emitted as a path-redacted DiagnosticEvent.
ContainmentResult RunContained(ContainedCall call, void* context,
                               const Deadline* deadline = nullptr,
                               DiagnosticStage stage = DiagnosticStage::Parse) noexcept;

} // namespace preview3d::provider
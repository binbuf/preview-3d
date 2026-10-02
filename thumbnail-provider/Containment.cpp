// T16 exception and structured-exception containment (see Containment.h).
//
// Compiled with synchronous C++ exception handling (/EHsc, set explicitly on
// this file) so `catch (...)` catches only C++ exceptions and a structured
// exception reaches the `__except` filter. PCH/COM-free so Tests.Unit.exe
// compiles the same source.

#include "Containment.h"
#include "ContainmentStage.h"

#include <windows.h>

#include <new>

namespace preview3d::provider {
namespace {

// Structured-exception code captured by the filter. A trivial thread_local has
// no destructor, so it never registers a TLS callback and cannot keep the
// module artificially loaded (design/05, "Threading and unload").
thread_local std::uint32_t t_sehCode = 0;

int SehFilter(EXCEPTION_POINTERS* info) noexcept
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (!ShouldContainStructuredCode(static_cast<std::uint32_t>(code))) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    t_sehCode = static_cast<std::uint32_t>(code);
    return EXCEPTION_EXECUTE_HANDLER;
}

// C++ exception boundary. Kept separate from the SEH frame so it can contain a
// try/catch without the compiler's "cannot use __try in a function that requires
// object unwinding" restriction. Catches everything, so it is safe to mark
// noexcept.
ProviderOutcome CppBoundary(ContainedCall call, void* context, bool* cppThrew,
                            bool* outOfMemory) noexcept
{
    *cppThrew = false;
    *outOfMemory = false;
    try {
        return call(context);
    } catch (const std::bad_alloc&) {
        *cppThrew = true;
        *outOfMemory = true;
        return ProviderOutcome::OutOfMemory;
    } catch (...) {
        *cppThrew = true;
        return ProviderOutcome::DecoderFailure;
    }
}

// Structured-exception boundary. Calls the C++ boundary so no C++ exception
// unwinds through the __try frame; only a real structured exception reaches the
// filter.
ProviderOutcome SehBoundary(ContainedCall call, void* context, bool* cppThrew,
                           bool* outOfMemory, std::uint32_t* sehCode) noexcept
{
    *sehCode = 0;
    t_sehCode = 0;
    __try {
        return CppBoundary(call, context, cppThrew, outOfMemory);
    } __except (SehFilter(GetExceptionInformation())) {
        *sehCode = t_sehCode;
        return ProviderOutcome::DecoderFailure;
    }
}

} // namespace

bool ShouldContainStructuredCode(std::uint32_t code) noexcept
{
    switch (code) {
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_BREAKPOINT:
        case EXCEPTION_SINGLE_STEP:
            return false;
        default:
            return true;
    }
}

ContainmentResult RunContained(ContainedCall call, void* context,
                               const Deadline* deadline,
                               DiagnosticStage stage) noexcept
{
    ContainmentResult result;

    const auto start = Deadline::Clock::now();
    bool cppThrew = false;
    bool outOfMemory = false;
    std::uint32_t sehCode = 0;
    result.outcome = SehBoundary(call, context, &cppThrew, &outOfMemory, &sehCode);
    const auto end = Deadline::Clock::now();

    result.cppException = cppThrew;
    result.structuredException = sehCode != 0;
    result.structuredCode = sehCode;
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    result.overranStop = deadline != nullptr && deadline->expired(end);

    // An uninterruptible call that returned after the stop point is rejected
    // after the fact; the in-call work is not interrupted.
    if (result.outcome == ProviderOutcome::Success && result.overranStop) {
        result.outcome = ProviderOutcome::Deadline;
    }

    DiagnosticEvent event{};
    event.stage = stage;
    event.outcome = result.outcome;
    event.elapsedMs = static_cast<std::uint32_t>(result.elapsed.count());
    event.overranStop = result.overranStop;
    event.cppException = result.cppException;
    event.structuredException = result.structuredException;
    event.structuredCode = result.structuredCode;
    Diagnostics::Emit(event);

    return result;
}

ErrorCode RunContainedStage(ContainedStageCall call, void* context,
                            DiagnosticStage stage) noexcept
{
    // Context for the fixed thunk: the adapter's stage function and its opaque
    // context, plus the typed code the function returned on a normal exit.
    struct StageContext {
        ContainedStageCall call = nullptr;
        void* context = nullptr;
        ErrorCode code = ErrorCode::InternalImporterFailure;
    };
    StageContext state{call, context, ErrorCode::InternalImporterFailure};

    const ContainmentResult contained = RunContained(
        [](void* raw) -> ProviderOutcome {
            auto* stageContext = static_cast<StageContext*>(raw);
            stageContext->code = stageContext->call(stageContext->context);
            return ClassifyError(stageContext->code);
        },
        &state, nullptr, stage);

    // A throw was translated by the boundary; recover the precise typed code.
    if (contained.cppException || contained.structuredException) {
        return contained.outcome == ProviderOutcome::OutOfMemory
                   ? ErrorCode::OutOfMemory
                   : ErrorCode::InternalImporterFailure;
    }
    return state.code;
}

} // namespace preview3d::provider
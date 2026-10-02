#pragma once

// T06 per-adapter stage containment.
//
// Every family adapter's frozen `IFamilyAdapter` method is `noexcept`. Its body
// performs product-owned allocation (std::vector/std::string/std::make_unique),
// and a `std::bad_alloc` raised inside a `noexcept` function calls
// `std::terminate` before the COM-boundary `RunContained` (Containment.h) can
// translate it. An adapter therefore routes each allocating stage through
// `RunContainedStage`, exactly as the STEP adapter routes its opaque OCCT calls
// through `RunContained`.
//
// This header is deliberately Windows-free: it declares only the typed entry
// point and a small thunk template, so an adapter that includes it does not pull
// `windows.h` (and its macros) into a translation unit that also includes a
// vendored parser header. The implementation lives in Containment.cpp, which is
// compiled `/EHsc`; a C++ throw from the stage unwinds across the module
// boundary into the containment boundary there and never escapes the adapter's
// `noexcept` method.

#include "DiagnosticStage.h"
#include "ProviderTypes.h"

namespace preview3d::provider {

// A product-owned adapter stage: it returns a typed error code and may throw a
// C++ exception (notably `std::bad_alloc`). It is intentionally NOT noexcept.
using ContainedStageCall = ErrorCode (*)(void* context);

// Runs `call(context)` under the T16 exception/structured-exception boundary and
// returns the typed `ErrorCode`:
//   - the callable's own returned code is passed through unchanged;
//   - a contained `std::bad_alloc` becomes `ErrorCode::OutOfMemory`;
//   - any other C++ exception or a contained structured exception becomes
//     `ErrorCode::InternalImporterFailure`.
// The stage is recorded by the boundary's path-redacted diagnostic event. The
// boundary does not rewrite a successful result for a passed stop point, so the
// stage's own cooperative deadline checks (and the pipeline's between-stage
// `Checkpoint()`) keep their exact semantics.
ErrorCode RunContainedStage(ContainedStageCall call, void* context,
                            DiagnosticStage stage) noexcept;

namespace detail {

template <typename Callable>
struct StageThunk {
    static ErrorCode Invoke(void* context)
    {
        return (*static_cast<Callable*>(context))();
    }
};

} // namespace detail

// Adapts a (capturing) callable returning `ErrorCode` into a `ContainedStageCall`.
// The callable is held by value for the duration of the contained call.
template <typename Callable>
ErrorCode RunContainedStageMember(Callable callable, DiagnosticStage stage) noexcept
{
    return RunContainedStage(&detail::StageThunk<Callable>::Invoke, &callable, stage);
}

} // namespace preview3d::provider
#pragma once

// T06 provider-host allocation-failure injection.
//
// The provider-host executable compiles the real family adapters, so replacing
// the executable's global `operator new` lets a test force a `std::bad_alloc`
// from a product-owned allocation inside an adapter stage. The adapter's frozen
// stage is routed through the T06 `RunContainedStage` boundary, which must
// translate it to `ErrorCode::OutOfMemory` instead of letting it escape a
// `noexcept` method and terminate the surrogate.
//
// The replacement is transparent and disarm-by-default: outside an armed scope
// it mirrors the default allocator (`malloc`/`free`), so every other host case
// is unaffected. The replacement lives in FaultInjectingAllocator.cpp.

namespace preview3d::test {

bool AllocationFailureArmed() noexcept;
void SetAllocationFailureArmed(bool armed) noexcept;

// Arms allocation failure for the enclosing scope; the destructor disarms it.
class ScopedAllocationFailure {
public:
    ScopedAllocationFailure() noexcept { SetAllocationFailureArmed(true); }
    ~ScopedAllocationFailure() noexcept { SetAllocationFailureArmed(false); }

    ScopedAllocationFailure(const ScopedAllocationFailure&) = delete;
    ScopedAllocationFailure& operator=(const ScopedAllocationFailure&) = delete;
};

} // namespace preview3d::test
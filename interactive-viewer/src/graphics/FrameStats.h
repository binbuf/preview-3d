#pragma once

// CPU-side frame timing for the D3D12 render path.
//
// Deliberately a stopwatch, not the ETW event schema
// .docs/design/04-rendering-and-streaming.md:194-203 specifies -- that is
// verification-infrastructure work for a much later batch. This exists to
// answer two narrow questions: "is the CPU actually running ahead of the GPU
// now" (the frame-pacing chunk) and "what does the D2D overlay cost per
// frame" (the ADR-010 spike).
//
// Lives under src/graphics rather than beside D3D12ViewerPath because it has
// no D3D dependency beyond an HRESULT, and src/graphics is on Tests.Unit's
// include path -- so the arithmetic is unit-testable without dragging the
// whole viewer path into the test project.

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>

class FrameStats
{
public:
    static constexpr size_t kCapacity = 240; // ~2 s of history at 120 Hz

    // Call once per presented frame, with Present's real HRESULT. The first
    // call only seeds the clock -- an interval needs two frames.
    void RecordPresent(HRESULT presentResult);

    // Test seam: same bookkeeping, with the interval supplied rather than
    // measured, so the statistics can be proven against known input.
    void RecordIntervalForTest(double intervalMs, HRESULT presentResult);

    void Reset();

    double MeanMs() const;
    // Nearest-rank p95 over the retained window. Returns 0 when no interval
    // has been recorded yet.
    double P95Ms() const;

    uint64_t PresentedFrames() const noexcept { return presentedFrames_; }
    // DXGI_STATUS_OCCLUDED is a SUCCEEDED() code, not a failure -- an
    // occluded frame still counts as presented, but its interval says more
    // about the compositor than about our own cost.
    uint64_t OccludedPresents() const noexcept { return occludedPresents_; }
    size_t SampleCount() const noexcept { return count_; }

private:
    void PushInterval(double intervalMs);
    void CountPresent(HRESULT presentResult);

    std::chrono::steady_clock::time_point last_{};
    bool hasLast_ = false;
    double intervalsMs_[kCapacity]{};
    size_t count_ = 0;
    size_t next_ = 0;
    uint64_t presentedFrames_ = 0;
    uint64_t occludedPresents_ = 0;
};

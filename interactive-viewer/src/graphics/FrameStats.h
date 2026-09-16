#pragma once

// CPU-side frame timing for the D3D12 render path.
//
// This remains the bounded in-process Present-return stopwatch. TSK-302 keeps
// its raw intervals and correlates/classifies compositor events through the
// opt-in PresentMon ETW qualification wrapper; CPU intervals are not mislabeled
// as scan-out timings. It also answers what the D2D overlay costs per frame.
//
// Lives under src/graphics rather than beside D3D12ViewerPath because it has
// no D3D dependency beyond an HRESULT, and src/graphics is on Tests.Unit's
// include path -- so the arithmetic is unit-testable without dragging the
// whole viewer path into the test project.

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

class FrameStats
{
public:
    // Bounded, but large enough to retain a one-minute 144 Hz qualification
    // run. Benchmark output keeps the raw intervals; ordinary UI statistics
    // still publish only scalar summaries.
    static constexpr size_t kCapacity = 16'384;

    // Call once per presented frame, with Present's real HRESULT. The first
    // call only seeds the clock -- an interval needs two frames.
    void RecordPresent(HRESULT presentResult);

    // Test seam: same bookkeeping, with the interval supplied rather than
    // measured, so the statistics can be proven against known input.
    void RecordIntervalForTest(double intervalMs, HRESULT presentResult);

    void Reset();

    double MeanMs() const;
    double MedianMs() const;
    // Nearest-rank p95 over the retained window. Returns 0 when no interval
    // has been recorded yet.
    double P95Ms() const;
    double MaxMs() const;

    // Chronological snapshot of the retained ring. This is intentionally
    // called only when a benchmark completes, never in the frame path.
    std::vector<double> RawIntervalsMs() const;

    uint64_t PresentedFrames() const noexcept { return presentedFrames_; }
    // DXGI_STATUS_OCCLUDED is a SUCCEEDED() code, not a failure -- an
    // occluded frame still counts as presented, but its interval says more
    // about the compositor than about our own cost.
    uint64_t OccludedPresents() const noexcept { return occludedPresents_; }
    uint64_t FailedPresents() const noexcept { return failedPresents_; }
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
    uint64_t failedPresents_ = 0;
};

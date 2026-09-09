#include "FrameStats.h"

#include <dxgi.h>

#include <algorithm>
#include <vector>

void FrameStats::CountPresent(HRESULT presentResult)
{
    ++presentedFrames_;
    if (presentResult == DXGI_STATUS_OCCLUDED) {
        ++occludedPresents_;
    }
}

void FrameStats::PushInterval(double intervalMs)
{
    intervalsMs_[next_] = intervalMs;
    next_ = (next_ + 1) % kCapacity;
    if (count_ < kCapacity) ++count_;
}

void FrameStats::RecordPresent(HRESULT presentResult)
{
    CountPresent(presentResult);

    const auto now = std::chrono::steady_clock::now();
    if (hasLast_) {
        const std::chrono::duration<double, std::milli> interval = now - last_;
        PushInterval(interval.count());
    }
    last_ = now;
    hasLast_ = true;
}

void FrameStats::RecordIntervalForTest(double intervalMs, HRESULT presentResult)
{
    CountPresent(presentResult);
    PushInterval(intervalMs);
}

void FrameStats::Reset()
{
    *this = FrameStats{};
}

double FrameStats::MeanMs() const
{
    if (count_ == 0) return 0.0;
    double total = 0.0;
    for (size_t i = 0; i < count_; ++i) total += intervalsMs_[i];
    return total / static_cast<double>(count_);
}

double FrameStats::P95Ms() const
{
    if (count_ == 0) return 0.0;
    // Copy and sort rather than maintaining order incrementally: the window
    // is at most kCapacity entries and this runs only when someone asks for
    // the number, never per frame.
    std::vector<double> sorted(intervalsMs_, intervalsMs_ + count_);
    std::sort(sorted.begin(), sorted.end());
    const size_t index = static_cast<size_t>(0.95 * static_cast<double>(count_ - 1) + 0.5);
    return sorted[index];
}

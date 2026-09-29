#pragma once

// T06 monotonic deadline.
//
// design/05-thumbnail-provider.md (call contract) fixes two time figures for
// every GetThumbnail call:
//   - a 750 ms p95 target, and
//   - a 2 s cooperative stop point checked at bounded parser/sampler/raster
//     intervals.
//
// `Deadline` is read from a monotonic clock (`std::chrono::steady_clock`), so it
// is immune to wall-clock adjustments and cheap enough to poll at each bounded
// checkpoint. `expired()` reports the cooperative stop point; `remaining()`
// reports the time left before it; `overTarget()` reports the p95 target.
//
// Contract: `Checkpoint()` is cooperative. It only observes that the stop point
// has passed at a point where the caller chose to check. Input preflight
// (counts/bytes) bounds how much input is admitted; it does NOT bound the time
// spent inside a third-party library call on the Shell's calling thread, which
// cannot be interrupted mid-call. An overrun that returns late must fail to the
// generic icon and be measured/reported; it must not be described as a hard
// wall-clock timeout (design/05; testing-strategy.md "Performance and memory
// method").
//
// The `now` parameters exist so deadline arithmetic is unit-testable without
// sleeping; production callers use the default.

#include <chrono>
#include <cstdint>

namespace preview3d::provider {

class Deadline {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    // 750 ms p95 target and the cooperative 2 s stop point (design/05).
    static constexpr std::chrono::milliseconds kTargetP95{750};
    static constexpr std::chrono::milliseconds kCooperativeStop{2000};

    // Starts a call deadline at the current monotonic instant.
    Deadline() noexcept : Deadline(Clock::now()) {}

    // Starts a call deadline at an explicit instant with the frozen figures by
    // default. Tests pass a fabricated `start`.
    explicit Deadline(TimePoint start,
                      std::chrono::milliseconds target = kTargetP95,
                      std::chrono::milliseconds stop = kCooperativeStop) noexcept
        : start_(start), target_(target), stop_(stop) {}

    // Restarts the same deadline policy at `now` (for a new call or phase).
    void Restart(TimePoint now = Clock::now()) noexcept { start_ = now; }

    // Time since the deadline started.
    Duration elapsed(TimePoint now = Clock::now()) const noexcept
    {
        return now - start_;
    }

    // True once the 750 ms p95 target has passed. Informational: the call keeps
    // going (within the stop point) but the elapsed time is out of target.
    bool overTarget(TimePoint now = Clock::now()) const noexcept
    {
        return elapsed(now) >= target_;
    }

    // True once the cooperative 2 s stop point has been reached. Callers must
    // stop promptly when this returns true.
    bool expired(TimePoint now = Clock::now()) const noexcept
    {
        return elapsed(now) >= stop_;
    }

    // Time remaining before the cooperative stop point; zero once expired.
    std::chrono::milliseconds remaining(TimePoint now = Clock::now()) const noexcept
    {
        if (expired(now)) {
            return std::chrono::milliseconds{0};
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(stop_ - elapsed(now));
    }

    // Time remaining before the p95 target; zero once over target.
    std::chrono::milliseconds remainingToTarget(
        TimePoint now = Clock::now()) const noexcept
    {
        if (overTarget(now)) {
            return std::chrono::milliseconds{0};
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(target_ - elapsed(now));
    }

    // Bounded-interval checkpoint. Returns false when the cooperative stop point
    // has passed and the caller should stop; call it between parse/sample/tile
    // units, never as a hard interrupt.
    bool Checkpoint(TimePoint now = Clock::now()) const noexcept
    {
        return !expired(now);
    }

    std::chrono::milliseconds target() const noexcept { return target_; }
    std::chrono::milliseconds stop() const noexcept { return stop_; }

private:
    TimePoint start_;
    std::chrono::milliseconds target_;
    std::chrono::milliseconds stop_;
};

} // namespace preview3d::provider
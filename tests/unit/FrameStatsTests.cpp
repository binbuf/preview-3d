// The CPU-side frame stopwatch the D3D12 path feeds from Present. Its
// arithmetic is testable in isolation because FrameStats has no D3D
// dependency beyond an HRESULT -- see its header for why it lives under
// src/graphics rather than beside D3D12ViewerPath.

#include "FrameStats.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

TEST_CASE("A fresh FrameStats reports nothing", "[graphics]")
{
    FrameStats stats;

    CHECK(stats.PresentedFrames() == 0);
    CHECK(stats.OccludedPresents() == 0);
    CHECK(stats.SampleCount() == 0);
    // No interval has been recorded, so there is no average to report --
    // 0 rather than a division by zero.
    CHECK(stats.MeanMs() == 0.0);
    CHECK(stats.P95Ms() == 0.0);
}

TEST_CASE("The first present seeds the clock without producing an interval", "[graphics]")
{
    FrameStats stats;
    stats.RecordPresent(S_OK);

    CHECK(stats.PresentedFrames() == 1);
    // An interval needs two frames.
    CHECK(stats.SampleCount() == 0);
}

TEST_CASE("Mean and p95 are computed over the recorded intervals", "[graphics]")
{
    FrameStats stats;
    // 1..100 ms. Mean is 50.5; nearest-rank p95 over 100 sorted samples is
    // index round(0.95 * 99) = 94, i.e. the 95th smallest, which is 95 ms.
    for (int i = 1; i <= 100; ++i) {
        stats.RecordIntervalForTest(static_cast<double>(i), S_OK);
    }

    CHECK(stats.SampleCount() == 100);
    CHECK(stats.MeanMs() > 50.4);
    CHECK(stats.MeanMs() < 50.6);
    CHECK(stats.P95Ms() == 95.0);
}

TEST_CASE("The interval ring keeps only its most recent window", "[graphics]")
{
    FrameStats stats;
    // Fill with a value that must be evicted, then overfill with another.
    for (size_t i = 0; i < FrameStats::kCapacity; ++i) {
        stats.RecordIntervalForTest(1.0, S_OK);
    }
    for (size_t i = 0; i < FrameStats::kCapacity; ++i) {
        stats.RecordIntervalForTest(9.0, S_OK);
    }

    CHECK(stats.SampleCount() == FrameStats::kCapacity);
    // Every 1.0 has been overwritten, so the window is entirely 9.0.
    CHECK(stats.MeanMs() == 9.0);
    // The frame counter is a lifetime total and is not bounded by the ring.
    CHECK(stats.PresentedFrames() == FrameStats::kCapacity * 2);
}

TEST_CASE("Occluded presents are counted but still count as presented", "[graphics]")
{
    FrameStats stats;
    stats.RecordIntervalForTest(8.0, S_OK);
    stats.RecordIntervalForTest(8.0, DXGI_STATUS_OCCLUDED);
    stats.RecordIntervalForTest(8.0, S_OK);

    CHECK(stats.PresentedFrames() == 3);
    // DXGI_STATUS_OCCLUDED is a SUCCEEDED() code -- the frame was presented,
    // it just went nowhere visible.
    CHECK(stats.OccludedPresents() == 1);
}

TEST_CASE("Reset clears both the interval window and the lifetime counters", "[graphics]")
{
    FrameStats stats;
    stats.RecordIntervalForTest(8.0, DXGI_STATUS_OCCLUDED);
    stats.RecordIntervalForTest(8.0, S_OK);
    REQUIRE(stats.SampleCount() == 2);

    stats.Reset();

    CHECK(stats.SampleCount() == 0);
    CHECK(stats.PresentedFrames() == 0);
    CHECK(stats.OccludedPresents() == 0);
    CHECK(stats.MeanMs() == 0.0);
}

TEST_CASE("Measured intervals track real elapsed time between presents", "[graphics]")
{
    // Proves RecordPresent's own clock, not just the arithmetic the
    // RecordIntervalForTest cases above cover.
    FrameStats stats;
    stats.RecordPresent(S_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stats.RecordPresent(S_OK);

    REQUIRE(stats.SampleCount() == 1);
    // Generous upper bound: this is a scheduler-dependent sleep, and the
    // point is that the clock runs at all, not that it is precise.
    CHECK(stats.MeanMs() >= 15.0);
    CHECK(stats.MeanMs() < 500.0);
}

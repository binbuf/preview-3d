// Gate 2 workstream B, slice 3: the generation/cancellation primitive
// Gate 0 originally promised (.docs/design/10-delivery-plan.md) but which
// stayed unbuilt until the upload ring's fence-complete publication path
// needed a real "is this still current" check.

#include <platform/Generation.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

using platform::GenerationSource;
using platform::GenerationToken;

TEST_CASE("A fresh token from a fresh source is current", "[platform]")
{
    GenerationSource source;
    GenerationToken token = source.Snapshot();
    CHECK(token.IsCurrent(source));
}

TEST_CASE("Advance invalidates a previously-snapshotted token", "[platform]")
{
    GenerationSource source;
    GenerationToken before = source.Snapshot();

    source.Advance();

    CHECK_FALSE(before.IsCurrent(source));
}

TEST_CASE("A token snapshotted after Advance is current, the earlier one is not", "[platform]")
{
    GenerationSource source;
    GenerationToken before = source.Snapshot();
    source.Advance();
    GenerationToken after = source.Snapshot();

    CHECK(after.IsCurrent(source));
    CHECK_FALSE(before.IsCurrent(source));
}

TEST_CASE("Advance returns strictly increasing values starting after 1", "[platform]")
{
    GenerationSource source;
    CHECK(source.Current() == 1);

    uint64_t first = source.Advance();
    uint64_t second = source.Advance();
    CHECK(first == 2);
    CHECK(second == 3);
}

TEST_CASE("Concurrent Advance calls never lose an update and Current never observes a torn value",
          "[platform]")
{
    GenerationSource source;
    constexpr int kThreads = 8;
    constexpr int kAdvancesPerThread = 1000;

    std::atomic<bool> sawDecrease{ false };
    std::atomic<bool> stopPolling{ false };

    // A poller keeps calling Current() concurrently with the advancers --
    // it must only ever observe values the counter genuinely held (no
    // torn 64-bit read) and the sequence it sees must never go backwards,
    // since Advance() only ever increments.
    std::thread poller([&] {
        uint64_t previous = source.Current();
        while (!stopPolling.load(std::memory_order_relaxed)) {
            uint64_t now = source.Current();
            if (now < previous) {
                sawDecrease.store(true, std::memory_order_relaxed);
            }
            previous = now;
        }
    });

    std::vector<std::thread> advancers;
    advancers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        advancers.emplace_back([&] {
            for (int j = 0; j < kAdvancesPerThread; ++j) {
                source.Advance();
            }
        });
    }
    for (auto& t : advancers) {
        t.join();
    }
    stopPolling.store(true, std::memory_order_relaxed);
    poller.join();

    CHECK_FALSE(sawDecrease.load());
    CHECK(source.Current() == 1 + static_cast<uint64_t>(kThreads) * kAdvancesPerThread);
}

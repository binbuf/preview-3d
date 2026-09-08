#include "D3D12CommandQueue.h"
#include "D3D12Device.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

namespace {

// A short-lived device shared by every test case in this file -- device
// creation is the expensive part; queue/fence creation against an
// already-created device is cheap.
D3D12Device& SharedDevice()
{
    static D3D12Device device = [] {
        D3D12Device d;
        auto result = d.Initialize();
        REQUIRE(result.success);
        return d;
    }();
    return device;
}

} // namespace

TEST_CASE("A fresh fence's CompletedValue is 0 before any signal", "[graphics]")
{
    D3D12CommandQueue queue;
    REQUIRE(queue.Initialize(*SharedDevice().Device(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"Test"));

    CHECK(queue.CompletedValue() == 0);
}

TEST_CASE("SignalNext returns increasing values that CompletedValue eventually reaches",
          "[graphics]")
{
    D3D12CommandQueue queue;
    REQUIRE(queue.Initialize(*SharedDevice().Device(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"Test"));

    uint64_t first = queue.SignalNext();
    uint64_t second = queue.SignalNext();
    CHECK(first == 1);
    CHECK(second == 2);

    // Signal completion isn't necessarily synchronous with Signal()
    // returning, even though it's fast -- poll with a short bounded loop
    // rather than asserting immediately.
    bool reached = false;
    for (int i = 0; i < 500 && !reached; ++i) {
        if (queue.CompletedValue() >= second) {
            reached = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK(reached);
}

TEST_CASE("WaitForValue on an already-reached value returns Signaled near-instantly", "[graphics]")
{
    D3D12CommandQueue queue;
    REQUIRE(queue.Initialize(*SharedDevice().Device(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"Test"));

    uint64_t value = queue.SignalNext();
    // Wait for it to genuinely complete first (via the poll path), so this
    // test proves WaitForValue's short-circuit branch specifically, not
    // its event-wait branch.
    for (int i = 0; i < 500 && queue.CompletedValue() < value; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(queue.CompletedValue() >= value);

    auto start = std::chrono::steady_clock::now();
    auto result = queue.WaitForValue(value, 5000);
    auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(result == D3D12CommandQueue::WaitResult::Signaled);
    CHECK(elapsed < std::chrono::milliseconds(500));
}

TEST_CASE("WaitForValue times out for a value nothing will ever signal", "[graphics]")
{
    D3D12CommandQueue queue;
    REQUIRE(queue.Initialize(*SharedDevice().Device(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"Test"));

    // Deliberately short: proving a timeout, not waiting one out. 200ms
    // comfortably exceeds fence-signal latency (sub-millisecond), so this
    // isn't flaky.
    auto result = queue.WaitForValue(/*value=*/100, /*timeoutMs=*/200);
    CHECK(result == D3D12CommandQueue::WaitResult::TimedOut);
}

TEST_CASE("A blocked WaitForValue genuinely unblocks when another thread signals it", "[graphics]")
{
    D3D12CommandQueue queue;
    REQUIRE(queue.Initialize(*SharedDevice().Device(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"Test"));

    std::thread signaler([&queue] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        queue.SignalNext();
    });

    auto result = queue.WaitForValue(/*value=*/1, /*timeoutMs=*/5000);
    signaler.join();

    CHECK(result == D3D12CommandQueue::WaitResult::Signaled);
}

TEST_CASE("Direct and copy queues from the same device signal independent fence sequences",
          "[graphics]")
{
    D3D12CommandQueue direct;
    D3D12CommandQueue copy;
    REQUIRE(direct.Initialize(*SharedDevice().Device(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"Direct"));
    REQUIRE(copy.Initialize(*SharedDevice().Device(), D3D12_COMMAND_LIST_TYPE_COPY, L"Copy"));

    uint64_t directValue = direct.SignalNext();
    uint64_t copyValue = copy.SignalNext();
    CHECK(directValue == 1);
    CHECK(copyValue == 1);

    CHECK(direct.WaitForValue(directValue, 5000) == D3D12CommandQueue::WaitResult::Signaled);
    CHECK(copy.WaitForValue(copyValue, 5000) == D3D12CommandQueue::WaitResult::Signaled);

    // Advancing one queue's fence must not affect the other's.
    direct.SignalNext();
    direct.SignalNext();
    CHECK(copy.CompletedValue() <= copyValue);
}

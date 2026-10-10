// T16 threading, deadline and containment coverage.
//
// ModuleLifetime.cpp, Diagnostics.cpp and Containment.cpp are compiled directly
// into Tests.Unit.exe (PCH-free, like StreamSource.cpp/ThumbnailPipeline.cpp), so
// the shipped lifetime counters, diagnostic gate and exception/structured-
// exception boundary are exercised as source, not as copies:
//
//   - an in-flight ActiveCallGuard keeps DllCanUnloadNow at S_FALSE even after
//     the last object reference is released, including from another thread;
//   - an injected C++ exception, std::bad_alloc and a raised/real access
//     violation are all translated to the tabulated HRESULT and never escape;
//   - stack-overflow/breakpoint/single-step are deliberately not swallowed;
//   - an uninterruptible call that returned after the cooperative stop point is
//     rejected after the fact with the real elapsed time recorded;
//   - diagnostics are off by default, enabled only explicitly, and the fixed
//     event text contains no path separator.

#include <catch2/catch_test_macros.hpp>

#include "Containment.h"
#include "Deadline.h"
#include "Diagnostics.h"
#include "ModuleLifetime.h"
#include "ProviderErrors.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

using namespace preview3d::provider;

namespace {

ProviderOutcome ThrowingCall(void*)
{
    throw std::runtime_error("injected parser exception");
}

ProviderOutcome BadAllocCall(void*)
{
    throw std::bad_alloc();
}

ProviderOutcome RaisedStructuredCall(void*)
{
    ::RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return ProviderOutcome::Success;
}

ProviderOutcome GenuineFaultCall(void*)
{
    volatile int* pointer = nullptr;
    *pointer = 1;
    return ProviderOutcome::Success;
}

ProviderOutcome SuccessCall(void*)
{
    return ProviderOutcome::Success;
}

// Clears the SEC-08 quarantine before and after a case, so an injected fault
// does not leak process state into the next case in Tests.Unit.exe.
struct QuarantineReset {
    QuarantineReset() noexcept { ResetContainmentQuarantineForTest(); }
    ~QuarantineReset() noexcept { ResetContainmentQuarantineForTest(); }
};

} // namespace

TEST_CASE("an in-flight call keeps the module from unloading", "[provider][threading]")
{
    ModuleLifetime::AddObject();
    CHECK_FALSE(ModuleLifetime::CanUnloadNow());

    {
        ActiveCallGuard guard;
        CHECK(ModuleLifetime::ActiveCalls() >= 1u);

        // The external object reference goes away while the call is still in
        // flight; the active-call count alone must keep the module loaded.
        ModuleLifetime::ReleaseObject();
        CHECK_FALSE(ModuleLifetime::CanUnloadNow());
    }

    CHECK(ModuleLifetime::ActiveCalls() == 0u);
    CHECK(ModuleLifetime::CanUnloadNow());
}

TEST_CASE("a live lock keeps the module from unloading", "[provider][threading]")
{
    ModuleLifetime::AddLock();
    CHECK_FALSE(ModuleLifetime::CanUnloadNow());
    ModuleLifetime::ReleaseLock();
    CHECK(ModuleLifetime::CanUnloadNow());
}

TEST_CASE("unload stays false while another thread is in a call",
          "[provider][threading]")
{
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};

    std::thread worker([&entered, &release]() {
        ActiveCallGuard guard;
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    CHECK_FALSE(ModuleLifetime::CanUnloadNow());

    release.store(true, std::memory_order_release);
    worker.join();
    CHECK(ModuleLifetime::CanUnloadNow());
}

TEST_CASE("an injected C++ exception is contained as a decoder failure",
          "[provider][threading]")
{
    const ContainmentResult result = RunContained(&ThrowingCall, nullptr);

    CHECK(result.outcome == ProviderOutcome::DecoderFailure);
    CHECK(result.cppException);
    CHECK_FALSE(result.structuredException);
    CHECK(result.structuredCode == 0u);
    CHECK(HresultFor(result.outcome) == E_FAIL);
}

TEST_CASE("an injected allocation failure maps to E_OUTOFMEMORY",
          "[provider][threading]")
{
    const ContainmentResult result = RunContained(&BadAllocCall, nullptr);

    CHECK(result.outcome == ProviderOutcome::OutOfMemory);
    CHECK(result.cppException);
    CHECK_FALSE(result.structuredException);
    CHECK(HresultFor(result.outcome) == E_OUTOFMEMORY);
}

TEST_CASE("a raised structured exception is contained as a decoder failure",
          "[provider][threading]")
{
    QuarantineReset reset;
    const ContainmentResult result = RunContained(&RaisedStructuredCall, nullptr);

    CHECK(result.outcome == ProviderOutcome::DecoderFailure);
    CHECK(result.structuredException);
    CHECK(result.structuredCode ==
          static_cast<std::uint32_t>(EXCEPTION_ACCESS_VIOLATION));
    CHECK_FALSE(result.cppException);
    CHECK(HresultFor(result.outcome) == E_FAIL);
}

TEST_CASE("a genuine access violation is contained", "[provider][threading]")
{
    QuarantineReset reset;
    const ContainmentResult result = RunContained(&GenuineFaultCall, nullptr);

    CHECK(result.outcome == ProviderOutcome::DecoderFailure);
    CHECK(result.structuredException);
    CHECK(result.structuredCode ==
          static_cast<std::uint32_t>(EXCEPTION_ACCESS_VIOLATION));
    CHECK(HresultFor(result.outcome) == E_FAIL);
}

TEST_CASE("stack overflow and debugger exceptions are not swallowed",
          "[provider][threading]")
{
    CHECK_FALSE(ShouldContainStructuredCode(EXCEPTION_STACK_OVERFLOW));
    CHECK_FALSE(ShouldContainStructuredCode(EXCEPTION_BREAKPOINT));
    CHECK_FALSE(ShouldContainStructuredCode(EXCEPTION_SINGLE_STEP));

    CHECK(ShouldContainStructuredCode(EXCEPTION_ACCESS_VIOLATION));
    CHECK(ShouldContainStructuredCode(EXCEPTION_INT_DIVIDE_BY_ZERO));
}

TEST_CASE("a contained structured fault quarantines the process until reset",
          "[provider][threading][quarantine]")
{
    QuarantineReset reset;
    CHECK_FALSE(ContainmentQuarantined());
    CHECK(ContainmentQuarantineCode() == 0u);

    const ContainmentResult result = RunContained(&RaisedStructuredCall, nullptr);
    CHECK(result.structuredException);
    CHECK(ContainmentQuarantined());
    CHECK(ContainmentQuarantineCode() ==
          static_cast<std::uint32_t>(EXCEPTION_ACCESS_VIOLATION));

    // The first fault wins; a later fault code does not overwrite it.
    MarkContainmentQuarantined(
        static_cast<std::uint32_t>(EXCEPTION_INT_DIVIDE_BY_ZERO));
    CHECK(ContainmentQuarantineCode() ==
          static_cast<std::uint32_t>(EXCEPTION_ACCESS_VIOLATION));

    // A code the boundary refuses to swallow never reaches the quarantine.
    CHECK_FALSE(ShouldContainStructuredCode(EXCEPTION_STACK_OVERFLOW));

    ResetContainmentQuarantineForTest();
    CHECK_FALSE(ContainmentQuarantined());
    CHECK(ContainmentQuarantineCode() == 0u);
}

TEST_CASE("a returned call past the stop point is rejected after the fact",
          "[provider][threading]")
{
    const auto start = Deadline::Clock::now() - std::chrono::seconds(5);
    const Deadline expired(start, std::chrono::milliseconds(10),
                           std::chrono::milliseconds(10));
    const ContainmentResult overrun = RunContained(&SuccessCall, nullptr, &expired);

    CHECK(overrun.overranStop);
    CHECK(overrun.outcome == ProviderOutcome::Deadline);
    CHECK(overrun.elapsed.count() >= 0);
    CHECK(HresultFor(overrun.outcome) == HRESULT_FROM_WIN32(ERROR_TIMEOUT));

    const Deadline fresh;
    const ContainmentResult within = RunContained(&SuccessCall, nullptr, &fresh);
    CHECK_FALSE(within.overranStop);
    CHECK(within.outcome == ProviderOutcome::Success);
}

TEST_CASE("diagnostics are off by default and path-free when enabled",
          "[provider][threading]")
{
    struct Capture {
        int count = 0;
        DiagnosticEvent last{};
    };

    auto sink = [](void* context, const DiagnosticEvent& event) noexcept {
        auto* capture = static_cast<Capture*>(context);
        capture->count += 1;
        capture->last = event;
    };

    Capture capture;
    Diagnostics::SetSink(sink, &capture);
    Diagnostics::SetEnabled(false);

    (void)RunContained(&SuccessCall, nullptr, nullptr, DiagnosticStage::Parse);
    CHECK(capture.count == 0);

    Diagnostics::SetEnabled(true);
    (void)RunContained(&SuccessCall, nullptr, nullptr, DiagnosticStage::Parse);
    CHECK(capture.count == 1);
    CHECK(capture.last.stage == DiagnosticStage::Parse);
    CHECK(capture.last.outcome == ProviderOutcome::Success);

    char text[128] = {};
    const std::size_t length =
        Diagnostics::FormatDiagnostic(capture.last, text, sizeof text);
    CHECK(length > 0u);
    CHECK(length < sizeof text);

    const std::string formatted(text, length);
    CHECK(formatted.find('\\') == std::string::npos);
    CHECK(formatted.find('/') == std::string::npos);
    CHECK(formatted.find(':') == std::string::npos);

    Diagnostics::SetEnabled(false);
    Diagnostics::SetSink(nullptr, nullptr);
}
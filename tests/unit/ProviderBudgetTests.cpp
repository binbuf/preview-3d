// T06 provider budget, deadline and HRESULT coverage.
//
// These cases are the automated acceptance for the checked constants, the
// process-wide allocation ledger, monotonic deadline arithmetic, checked
// file-derived arithmetic and the HRESULT table. They run as part of the
// repository's baseline `x64\Release\Tests.Unit.exe` (and Debug) verify
// command; no provider DLL activation is involved because the T06 services are
// header-only and compiled directly into this test image.

#include <catch2/catch_test_macros.hpp>

#include "AllocationLedger.h"
#include "Deadline.h"
#include "ProviderErrors.h"
#include "ProviderLimits.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace preview3d::provider;
using namespace std::chrono_literals;

namespace {

// Every adapter-facing code, walked as a contiguous enum range so a new code
// added to model_core is forced through ClassifyError here.
constexpr std::uint32_t kFirstErrorCode =
    static_cast<std::uint32_t>(model_core::ImportErrorCode::None);
constexpr std::uint32_t kLastErrorCode =
    static_cast<std::uint32_t>(model_core::ImportErrorCode::TessellationFailed);

} // namespace

TEST_CASE("provider budget constants match the frozen caps", "[provider][budget]")
{
    STATIC_REQUIRE(ProviderLimits::kStreamMaxBytes == 256ull * 1024 * 1024);
    STATIC_REQUIRE(ProviderLimits::kContiguousBackingMaxBytes == 128ull * 1024 * 1024);
    STATIC_REQUIRE(ProviderLimits::kAccountedScratchMaxBytes == 192ull * 1024 * 1024);
    STATIC_REQUIRE(ProviderLimits::kAllocationLedgerMaxBytes == 384ull * 1024 * 1024);
    STATIC_REQUIRE(ProviderLimits::kProcessCommitQualificationTargetBytes ==
                   384ull * 1024 * 1024);
    STATIC_REQUIRE(ProviderLimits::kTrianglesInspectedMax == 2'000'000);
    STATIC_REQUIRE(ProviderLimits::kPointsInspectedMax == 6'000'000);
    STATIC_REQUIRE(ProviderLimits::kRasterizedSamplesMax == 250'000);
    STATIC_REQUIRE(ProviderLimits::kDecodedTexturePixelsMax == 32'000'000);
    STATIC_REQUIRE(ProviderLimits::kNodesMax == 10'000);
    STATIC_REQUIRE(ProviderLimits::kMaterialsMax == 4'096);
    STATIC_REQUIRE(ProviderLimits::kDracoDecodedWorkingSetMaxBytes ==
                   96ull * 1024 * 1024);
    STATIC_REQUIRE(ProviderLimits::kDracoTrianglesMax == 1'000'000);

    // The two 384 MiB figures are distinct contracts: one ledger-enforced, one a
    // measured target. They happen to share a value today.
    CHECK(ProviderLimits::Default().kAllocationLedgerMaxBytes ==
          ProviderLimits::kProcessCommitQualificationTargetBytes);
    CHECK(AllocationLedger::kDefaultLimitBytes == ProviderLimits::kAllocationLedgerMaxBytes);
}

TEST_CASE("checked file-derived arithmetic rejects overflow and out-of-range",
          "[provider][budget]")
{
    CHECK(CheckedAdd(1, 2) == 3u);
    CHECK(CheckedAdd(0, 0) == 0u);
    CHECK_FALSE(CheckedAdd(UINT64_MAX, 1).has_value());

    CHECK(CheckedMultiply(3, 4) == 12u);
    CHECK(CheckedMultiply(0, UINT64_MAX) == 0u);
    CHECK_FALSE(CheckedMultiply(UINT64_MAX, 2).has_value());
    CHECK_FALSE(CheckedMultiply(2, UINT64_MAX).has_value());

    CHECK(CheckedRangeEnd(10, 5) == 15u);
    CHECK_FALSE(CheckedRangeEnd(UINT64_MAX, 1).has_value());

    constexpr std::uint64_t kLimit = 100;
    CHECK(FitsInRange(0, 100, kLimit));
    CHECK(FitsInRange(50, 50, kLimit));
    CHECK(FitsInRange(100, 0, kLimit));
    CHECK_FALSE(FitsInRange(50, 51, kLimit));
    CHECK_FALSE(FitsInRange(101, 0, kLimit));
    // offset + size wraps: must fail even though it "looks" inside the limit.
    CHECK_FALSE(FitsInRange(UINT64_MAX, 2, kLimit));

    // Cap boundaries: a limit-sized range fits, one byte more does not.
    CHECK(FitsInRange(0, ProviderLimits::kStreamMaxBytes,
                      ProviderLimits::kStreamMaxBytes));
    CHECK_FALSE(FitsInRange(0, ProviderLimits::kStreamMaxBytes + 1,
                            ProviderLimits::kStreamMaxBytes));
    CHECK(FitsInRange(0, ProviderLimits::kContiguousBackingMaxBytes,
                      ProviderLimits::kContiguousBackingMaxBytes));
    CHECK_FALSE(FitsInRange(0, ProviderLimits::kContiguousBackingMaxBytes + 1,
                            ProviderLimits::kContiguousBackingMaxBytes));

    CHECK(CheckedNarrow<std::uint32_t>(UINT32_MAX) == UINT32_MAX);
    CHECK(CheckedNarrow<std::uint32_t>(0) == 0u);
    CHECK_FALSE(CheckedNarrow<std::uint32_t>(std::uint64_t{UINT32_MAX} + 1).has_value());
    CHECK(CheckedNarrow<std::int32_t>(static_cast<std::uint64_t>(INT32_MAX)) == INT32_MAX);
    CHECK_FALSE(CheckedNarrow<std::int32_t>(static_cast<std::uint64_t>(INT32_MAX) + 1)
                    .has_value());
}

TEST_CASE("allocation ledger rejects the charge that crosses the limit",
          "[provider][budget]")
{
    AllocationLedger ledger(100);
    CHECK(ledger.Limit() == 100u);
    CHECK(ledger.Reserved() == 0u);
    CHECK(ledger.Available() == 100u);

    CHECK(ledger.TryReserve(0));
    CHECK(ledger.Reserved() == 0u);

    CHECK(ledger.TryReserve(60));
    CHECK(ledger.Reserved() == 60u);
    CHECK(ledger.Available() == 40u);

    // A charge that would exactly reach the limit is allowed; one byte more is
    // rejected without changing the running total.
    CHECK(ledger.TryReserve(40));
    CHECK(ledger.Reserved() == 100u);
    CHECK_FALSE(ledger.TryReserve(1));
    CHECK(ledger.Reserved() == 100u);
    CHECK_FALSE(ledger.TryReserve(UINT64_MAX));
    CHECK(ledger.Reserved() == 100u);

    ledger.Release(40);
    CHECK(ledger.Reserved() == 60u);
    CHECK(ledger.TryReserve(40));

    ledger.Release(100);
    CHECK(ledger.Reserved() == 0u);
    CHECK_FALSE(ledger.TryReserve(101));
    CHECK(ledger.TryReserve(101) == false);

    // The process-wide ledger is shared and uses the frozen 384 MiB ceiling.
    CHECK(AllocationLedger::ProcessWide().Limit() ==
          ProviderLimits::kAllocationLedgerMaxBytes);
    CHECK(&AllocationLedger::ProcessWide() == &AllocationLedger::ProcessWide());
}

TEST_CASE("allocation ledger reserves atomically under concurrency",
          "[provider][budget]")
{
    constexpr std::uint64_t kCapacity = 1000;
    constexpr int kThreads = 8;
    constexpr int kAttemptsPerThread = 250; // 2000 attempts for 1000 units.

    AllocationLedger ledger(kCapacity);
    std::atomic<int> successes{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&ledger, &successes] {
            int local = 0;
            for (int i = 0; i < kAttemptsPerThread; ++i) {
                if (ledger.TryReserve(1)) {
                    ++local;
                }
                // The invariant must hold after every concurrent step.
                CHECK(ledger.Reserved() <= ledger.Limit());
            }
            successes.fetch_add(local, std::memory_order_relaxed);
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    // Exactly the capacity in one-byte charges must have been granted.
    CHECK(successes.load() == static_cast<int>(kCapacity));
    CHECK(ledger.Reserved() == kCapacity);
    CHECK(ledger.Available() == 0u);

    ledger.Release(kCapacity);
    CHECK(ledger.Reserved() == 0u);
}

TEST_CASE("scoped allocation reservation releases on destruction",
          "[provider][budget]")
{
    AllocationLedger ledger(100);
    {
        auto reservation = ledger.ReserveScoped(60);
        REQUIRE(reservation.has_value());
        CHECK(reservation->bytes() == 60u);
        CHECK(ledger.Reserved() == 60u);

        auto tooBig = ledger.ReserveScoped(50);
        CHECK_FALSE(tooBig.has_value());
        CHECK(ledger.Reserved() == 60u);

        // Move releases nothing; the moved-from token is empty.
        AllocationReservation moved = std::move(*reservation);
        CHECK(moved.bytes() == 60u);
        CHECK(ledger.Reserved() == 60u);
        moved.Release();
        CHECK(ledger.Reserved() == 0u);
    }
    CHECK(ledger.Reserved() == 0u);
}

TEST_CASE("monotonic deadline honours the target and cooperative stop",
          "[provider][budget]")
{
    using Clock = Deadline::Clock;
    const Clock::time_point start{};
    Deadline deadline(start, 750ms, 2000ms);
    const auto at = [&start](int ms) { return start + std::chrono::milliseconds(ms); };

    CHECK(deadline.target() == 750ms);
    CHECK(deadline.stop() == 2000ms);

    CHECK(deadline.elapsed(at(0)) == 0ms);
    CHECK_FALSE(deadline.overTarget(at(0)));
    CHECK_FALSE(deadline.expired(at(0)));
    CHECK(deadline.remaining(at(0)) == 2000ms);
    CHECK(deadline.remainingToTarget(at(0)) == 750ms);
    CHECK(deadline.Checkpoint(at(0)));

    CHECK_FALSE(deadline.overTarget(at(749)));
    CHECK_FALSE(deadline.expired(at(749)));

    CHECK(deadline.overTarget(at(750)));
    CHECK_FALSE(deadline.expired(at(750)));
    CHECK(deadline.remaining(at(750)) == 1250ms);
    CHECK(deadline.remainingToTarget(at(750)) == 0ms);
    CHECK(deadline.Checkpoint(at(750)));

    CHECK_FALSE(deadline.expired(at(1999)));
    CHECK(deadline.remaining(at(1999)) == 1ms);

    // The stop point is inclusive: at 2000 ms the deadline is expired.
    CHECK(deadline.expired(at(2000)));
    CHECK_FALSE(deadline.Checkpoint(at(2000)));
    CHECK(deadline.remaining(at(2000)) == 0ms);
    CHECK(deadline.expired(at(5000)));

    // Defaults are the frozen figures; Restart rebases the same policy.
    Deadline defaults(start);
    CHECK(defaults.target() == Deadline::kTargetP95);
    CHECK(defaults.stop() == Deadline::kCooperativeStop);
    defaults.Restart(at(1000));
    CHECK_FALSE(defaults.expired(at(2999)));
    CHECK(defaults.expired(at(3000)));
    CHECK(defaults.remaining(at(1000)) == 2000ms);
}

TEST_CASE("provider HRESULT mapping matches every table row", "[provider][budget]")
{
    using O = ProviderOutcome;
    CHECK(HresultFor(O::Success) == S_OK);
    CHECK(HresultFor(O::BadPointer) == E_POINTER);
    CHECK(HresultFor(O::InvalidCallSequence) == E_UNEXPECTED);
    CHECK(HresultFor(O::Unsupported) == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    CHECK(HresultFor(O::BadFormat) == HRESULT_FROM_WIN32(ERROR_BAD_FORMAT));
    CHECK(HresultFor(O::LimitExceeded) == HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
    CHECK(HresultFor(O::Deadline) == HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    CHECK(HresultFor(O::OutOfMemory) == E_OUTOFMEMORY);
    CHECK(HresultFor(O::DecoderFailure) == E_FAIL);
}

TEST_CASE("provider classifies every import error code", "[provider][budget]")
{
    using E = model_core::ImportErrorCode;
    using O = ProviderOutcome;

    // Spot-check each row against the adapter taxonomy.
    CHECK(ClassifyError(E::None) == O::Success);
    CHECK(ClassifyError(E::UnsupportedFormat) == O::Unsupported);
    CHECK(ClassifyError(E::UnsupportedEncoding) == O::Unsupported);
    CHECK(ClassifyError(E::UnsupportedRequiredFeature) == O::Unsupported);
    CHECK(ClassifyError(E::UnsafeReference) == O::Unsupported);
    CHECK(ClassifyError(E::MalformedData) == O::BadFormat);
    CHECK(ClassifyError(E::EmptyGeometry) == O::BadFormat);
    CHECK(ClassifyError(E::FileUnavailable) == O::BadFormat);
    CHECK(ClassifyError(E::ResourceLimit) == O::LimitExceeded);
    CHECK(ClassifyError(E::ScratchLimit) == O::LimitExceeded);
    CHECK(ClassifyError(E::DracoPrimitiveLimit) == O::LimitExceeded);
    CHECK(ClassifyError(E::ArchiveLimit) == O::LimitExceeded);
    CHECK(ClassifyError(E::Cancelled) == O::Deadline);
    CHECK(ClassifyError(E::WorkerTimedOut) == O::Deadline);
    CHECK(ClassifyError(E::OutOfMemory) == O::OutOfMemory);
    CHECK(ClassifyError(E::InternalImporterFailure) == O::DecoderFailure);
    CHECK(ClassifyError(E::WorkerCrashed) == O::DecoderFailure);
    CHECK(ClassifyError(E::TessellationFailed) == O::DecoderFailure);

    // Every code maps to exactly one tabulated HRESULT (composition is total).
    for (std::uint32_t raw = kFirstErrorCode; raw <= kLastErrorCode; ++raw) {
        const auto code = static_cast<E>(raw);
        const ProviderOutcome outcome = ClassifyError(code);
        const HRESULT hr = HresultForError(code);
        CHECK(hr == HresultFor(outcome));
        CHECK((hr == S_OK || hr == E_POINTER || hr == E_UNEXPECTED ||
               hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) ||
               hr == HRESULT_FROM_WIN32(ERROR_BAD_FORMAT) ||
               hr == HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE) ||
               hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT) ||
               hr == E_OUTOFMEMORY || hr == E_FAIL));
    }

    // Exact composed values for the documented conditions.
    CHECK(HresultForError(E::MalformedData) == HRESULT_FROM_WIN32(ERROR_BAD_FORMAT));
    CHECK(HresultForError(E::EmptyGeometry) == HRESULT_FROM_WIN32(ERROR_BAD_FORMAT));
    CHECK(HresultForError(E::UnsupportedRequiredFeature) ==
          HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    CHECK(HresultForError(E::ResourceLimit) ==
          HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
    CHECK(HresultForError(E::Cancelled) == HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    CHECK(HresultForError(E::OutOfMemory) == E_OUTOFMEMORY);
    CHECK(HresultForError(E::InternalImporterFailure) == E_FAIL);
}
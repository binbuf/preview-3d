#pragma once

// T06 process-wide product-owned allocation ledger.
//
// design/05-thumbnail-provider.md ("Stream ingestion") and design/03
// ("Hard limits") require a process-wide ledger that reserves product-owned
// allocations before they occur, including across concurrent GetThumbnail
// calls, and rejects any charge that would push the running total past 384 MiB.
// T12/T14/T15 and every adapter charge the allocations they control against
// `ProcessWide()` (or a caller-owned ledger); `AllocationReservation` releases
// a charge on scope exit.
//
// What the ledger does and does not cover (design/05, testing-strategy.md):
//   - decoder allocations that expose an allocator callback must be routed
//     through the ledger from the callback, so library bytes are charged too;
//   - library allocations without such a callback (e.g. TinyUSDZ-style
//     libraries), DLL/module loading, GDI/USER objects, and other Shell
//     surrogate overhead are NOT covered by this ledger and cannot be bounded
//     by it;
//   - therefore the 384 MiB figure here is the enforced ceiling on accounted
//     charges, while the 384 MiB total-process-private-commit figure above an
//     idle, loaded surrogate baseline is a measured qualification target
//     (ProviderLimits::kProcessCommitQualificationTargetBytes) that T51 records,
//     not something this ledger claims to enforce.
//
// Accounting is lock-free and atomic: a reservation either succeeds and updates
// the running total, or is rejected without changing it. The invariant
// `Reserved() <= Limit()` always holds, so aggregate charges never cross the
// limit even when many calls reserve concurrently.

#include "ProviderLimits.h"

#include <atomic>
#include <cstdint>
#include <optional>

namespace preview3d::provider {

class AllocationLedger;
class AllocationReservation;

// RAII token for one successful reservation. Move-only; releases its bytes back
// to the originating ledger on destruction. A default-constructed token holds
// nothing.
class AllocationReservation {
public:
    AllocationReservation() noexcept = default;
    AllocationReservation(const AllocationReservation&) = delete;
    AllocationReservation& operator=(const AllocationReservation&) = delete;

    AllocationReservation(AllocationReservation&& other) noexcept
        : ledger_(other.ledger_), bytes_(other.bytes_)
    {
        other.ledger_ = nullptr;
        other.bytes_ = 0;
    }

    AllocationReservation& operator=(AllocationReservation&& other) noexcept
    {
        if (this != &other) {
            Release();
            ledger_ = other.ledger_;
            bytes_ = other.bytes_;
            other.ledger_ = nullptr;
            other.bytes_ = 0;
        }
        return *this;
    }

    ~AllocationReservation() { Release(); }

    // Bytes held by this token (0 for an empty token).
    std::uint64_t bytes() const noexcept { return bytes_; }

    // Releases early; the token becomes empty and destructor release is a no-op.
    void Release() noexcept;

private:
    friend class AllocationLedger;
    AllocationReservation(AllocationLedger& ledger, std::uint64_t bytes) noexcept
        : ledger_(&ledger), bytes_(bytes)
    {
    }

    AllocationLedger* ledger_ = nullptr;
    std::uint64_t bytes_ = 0;
};

class AllocationLedger {
public:
    // The enforced ceiling for product-owned charges (design/05/03).
    static constexpr std::uint64_t kDefaultLimitBytes =
        ProviderLimits::kAllocationLedgerMaxBytes;

    // A caller-owned ledger with an explicit ceiling; tests use a small limit.
    // (Reserved byte count only, not a memory pool.)
    explicit AllocationLedger(std::uint64_t limit = kDefaultLimitBytes) noexcept
        : limit_(limit)
    {
    }

    AllocationLedger(const AllocationLedger&) = delete;
    AllocationLedger& operator=(const AllocationLedger&) = delete;

    // The single ledger shared by every provider call in this module. One copy
    // per loaded provider image, so concurrent Shell calls share the ceiling.
    static AllocationLedger& ProcessWide() noexcept
    {
        static AllocationLedger ledger;
        return ledger;
    }

    // Atomically reserves `bytes` if it fits. Returns false (and changes
    // nothing) when the charge would cross the limit. A zero-byte charge always
    // succeeds and is a no-op.
    bool TryReserve(std::uint64_t bytes) noexcept
    {
        if (bytes == 0) {
            return true;
        }
        std::uint64_t current = reserved_.load(std::memory_order_relaxed);
        for (;;) {
            if (bytes > limit_ - current) {
                return false;
            }
            if (reserved_.compare_exchange_weak(current, current + bytes,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    // Atomically releases a previously reserved charge. `bytes` must not exceed
    // the amount the caller reserved; a zero-byte release is a no-op.
    void Release(std::uint64_t bytes) noexcept
    {
        if (bytes == 0) {
            return;
        }
        reserved_.fetch_sub(bytes, std::memory_order_acq_rel);
    }

    // Reserves and returns an RAII token, or nullopt when the charge does not
    // fit.
    std::optional<AllocationReservation> ReserveScoped(
        std::uint64_t bytes) noexcept
    {
        if (!TryReserve(bytes)) {
            return std::nullopt;
        }
        return AllocationReservation(*this, bytes);
    }

    std::uint64_t Reserved() const noexcept
    {
        return reserved_.load(std::memory_order_acquire);
    }

    std::uint64_t Limit() const noexcept { return limit_; }

    std::uint64_t Available() const noexcept { return limit_ - Reserved(); }

private:
    std::atomic<std::uint64_t> reserved_{0};
    const std::uint64_t limit_;
};

inline void AllocationReservation::Release() noexcept
{
    if (ledger_ != nullptr && bytes_ != 0) {
        ledger_->Release(bytes_);
    }
    ledger_ = nullptr;
    bytes_ = 0;
}

} // namespace preview3d::provider
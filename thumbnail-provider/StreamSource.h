#pragma once

// T12 bounded stream backing over IInitializeWithStream.
//
// design/05-thumbnail-provider.md ("Call contract", "Stream ingestion") makes the
// Shell-managed IStream the only input the provider ever reads. This header owns
// the one implementation of the frozen `BoundedSource` interface (ProviderTypes.h,
// T04): a bounded, deadline-aware source that never trusts STATSTG, never assumes
// a filesystem path, resolves no sidecar and writes no temporary or persistent
// file.
//
// What the source guarantees:
//   - exactly one adopted stream, held by an independent reference; the caller
//     (the object created by the T11 factory) owns the source for its lifetime;
//   - a cheap preflight that fails fast with `LimitExceeded` when a reported size
//     exceeds the 256 MiB stream cap (ProviderLimits::kStreamMaxBytes), before a
//     single byte is read;
//   - serialized, checked range reads: every read is verified against the
//     reported/validated size and against the 256 MiB ceiling, and a short read
//     inside the reported extent is an inconsistency that aborts to the safe
//     fallback (`BadFormat`), never a partial trusted result;
//   - a small block cache for seek-capable streams (`kBlockBytes` x
//     `kBlockSlots`, charged to the T06 ledger before allocation);
//   - a checked contiguous backing buffer up to 128 MiB for non-seekable input
//     or adapters that need one buffer; a larger source returns an empty
//     `ContiguousView()` (`LimitExceeded`) instead of over-allocating;
//   - deadline abort at every bounded block/read/materialize step.
//
// This is the T12 Windows boundary ADR-0009 allows: `ProviderTypes.h` stays
// GDI/Windows-type-free and only this header names `IStream`/`STATSTG`.
//
// Threading: the object is apartment-affine like the COM object that owns it.
// Reads are serialized by the COM call model; the source adds no lock and no
// process-global mutable state beyond the T06 ledger it charges.

#include "AllocationLedger.h"
#include "Deadline.h"
#include "ProviderErrors.h"
#include "ProviderLimits.h"
#include "ProviderTypes.h"

#include <windows.h>

#include <objidl.h> // IStream, STATSTG

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace preview3d::provider {

// The single `BoundedSource` implementation, built over the Shell's IStream.
class BoundedStreamSource final : public BoundedSource {
public:
    // Adopts `stream` (takes its own reference), probes seekability, applies the
    // STATSTG/256 MiB preflight and reserves the block cache. On failure returns
    // nullptr and reports the row of the design/05 HRESULT table in `outcome`.
    //
    // A null `stream` reports `BadPointer` (the COM boundary maps that to
    // E_POINTER). `deadline` is copied into the source; the owning object can
    // restart it at GetThumbnail entry through MutableDeadline().
    static std::unique_ptr<BoundedStreamSource> Create(
        IStream* stream,
        Deadline deadline,
        AllocationLedger& ledger,
        ProviderOutcome& outcome) noexcept;

    ~BoundedStreamSource() noexcept override;

    BoundedStreamSource(const BoundedStreamSource&) = delete;
    BoundedStreamSource& operator=(const BoundedStreamSource&) = delete;

    // -- BoundedSource --------------------------------------------------------

    // Validated source size, or 0 while it is still unknown (a non-seekable
    // stream that has not been materialized yet). Never a trust decision.
    std::uint64_t Size() const noexcept override;

    bool Seekable() const noexcept override;

    // Copies exactly `dest.size()` bytes from absolute `offset`. Returns false on
    // a range outside the validated extent, a short-read inconsistency, a ledger
    // cap, an allocation failure or an expired deadline.
    bool ReadAt(std::uint64_t offset, std::span<std::byte> dest) override;

    // A const view of the whole bounded source when it fits the 128 MiB
    // contiguous backing cap, otherwise an empty span. Non-seekable input is
    // materialized on first use; a seekable source larger than the cap returns
    // empty rather than over-allocating.
    std::span<const std::byte> ContiguousView() override;

    // -- Introspection --------------------------------------------------------

    // Row of the HRESULT table behind the most recent failed call (Success after
    // a successful one).
    ProviderOutcome FailureCause() const noexcept { return failure_; }

    // The per-object read deadline. T13/T16 restart it at GetThumbnail entry so
    // stream reads share the call's cooperative budget.
    Deadline& MutableDeadline() noexcept { return deadline_; }

    // Frozen small-cache geometry (design/05, "seek-capable streams ... small
    // block cache"). Charged to the ledger before allocation.
    static constexpr std::uint64_t kBlockBytes = 64ull * 1024;
    static constexpr std::size_t kBlockSlots = 8;

private:
    BoundedStreamSource(IStream* stream, Deadline deadline,
                        AllocationLedger& ledger) noexcept;

    struct Block {
        std::uint64_t index = 0;
        std::size_t length = 0;
        bool valid = false;
        std::vector<std::byte> bytes;
    };

    bool Fail(ProviderOutcome outcome) noexcept;
    bool DetermineSize() noexcept;
    bool ReserveCache() noexcept;
    bool ReserveBacking(std::uint64_t total) noexcept;
    bool FillBlock(std::uint64_t index) noexcept;
    bool ReadViaCache(std::uint64_t offset, std::span<std::byte> dest) noexcept;
    bool Materialize() noexcept;

    IStream* stream_ = nullptr;
    Deadline deadline_;
    AllocationLedger* ledger_ = nullptr;
    AllocationReservation cacheReservation_;
    std::uint64_t backingReserved_ = 0;

    bool seekable_ = false;
    bool sizeKnown_ = false;
    std::uint64_t size_ = 0;

    std::vector<Block> cache_;
    std::vector<std::byte> backing_;
    bool backingReady_ = false;

    ProviderOutcome failure_ = ProviderOutcome::Success;
};

} // namespace preview3d::provider
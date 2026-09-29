// T12 bounded stream backing implementation (see StreamSource.h).
//
// Deliberately free of the provider precompiled header, like the shared
// parser-core sources: the same translation unit is compiled into both
// Preview3DThumbnailProvider.dll and Tests.Unit.exe (and it must stay free of a
// worker/broker/viewer header, per ADR-0004/ADR-0009).

#include "StreamSource.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace preview3d::provider {

namespace {
constexpr std::uint64_t kMaterializeChunkBytes = 256ull * 1024;
} // namespace

BoundedStreamSource::BoundedStreamSource(IStream* stream, Deadline deadline,
                                         AllocationLedger& ledger) noexcept
    : stream_(stream), deadline_(deadline), ledger_(&ledger)
{
    if (stream_ != nullptr) {
        stream_->AddRef();
    }
}

BoundedStreamSource::~BoundedStreamSource() noexcept
{
    if (ledger_ != nullptr && backingReserved_ != 0) {
        ledger_->Release(backingReserved_);
    }
    cacheReservation_.Release();
    if (stream_ != nullptr) {
        stream_->Release();
    }
}

std::unique_ptr<BoundedStreamSource> BoundedStreamSource::Create(
    IStream* stream, Deadline deadline, AllocationLedger& ledger,
    ProviderOutcome& outcome) noexcept
{
    if (stream == nullptr) {
        outcome = ProviderOutcome::BadPointer;
        return nullptr;
    }

    std::unique_ptr<BoundedStreamSource> source(
        new (std::nothrow) BoundedStreamSource(stream, deadline, ledger));
    if (!source) {
        outcome = ProviderOutcome::OutOfMemory;
        return nullptr;
    }

    // Seekability is advertised, never assumed: STREAM_SEEK_CUR with a null new
    // position queries the current position without moving the stream.
    LARGE_INTEGER zero{};
    zero.QuadPart = 0;
    ULARGE_INTEGER current{};
    source->seekable_ = SUCCEEDED(stream->Seek(zero, STREAM_SEEK_CUR, &current));

    if (!source->DetermineSize()) {
        outcome = source->failure_;
        return nullptr;
    }
    if (source->seekable_ && !source->ReserveCache()) {
        outcome = source->failure_;
        return nullptr;
    }

    outcome = ProviderOutcome::Success;
    return source;
}

bool BoundedStreamSource::Fail(ProviderOutcome outcome) noexcept
{
    failure_ = outcome;
    return false;
}

bool BoundedStreamSource::DetermineSize() noexcept
{
    STATSTG stat{};
    const HRESULT statHr = stream_->Stat(&stat, STATFLAG_NONAME);
    const bool statOk = SUCCEEDED(statHr) && stat.type == STGTY_STREAM;

    // Fail fast on a reported size over the stream cap, before any read. A
    // seek-to-end probe below does not override this: STATSTG over the ceiling
    // is exactly the cheap oversized-file check design/05 requires.
    if (statOk && stat.cbSize.QuadPart > ProviderLimits::kStreamMaxBytes) {
        return Fail(ProviderOutcome::LimitExceeded);
    }

    if (seekable_) {
        LARGE_INTEGER zero{};
        zero.QuadPart = 0;
        ULARGE_INTEGER end{};
        if (SUCCEEDED(stream_->Seek(zero, STREAM_SEEK_END, &end))) {
            if (end.QuadPart > ProviderLimits::kStreamMaxBytes) {
                return Fail(ProviderOutcome::LimitExceeded);
            }
            size_ = end.QuadPart;
            sizeKnown_ = true;
            if (FAILED(stream_->Seek(zero, STREAM_SEEK_SET, nullptr))) {
                return Fail(ProviderOutcome::Unsupported);
            }
        } else if (statOk) {
            size_ = stat.cbSize.QuadPart;
            sizeKnown_ = true;
        }
    } else if (statOk) {
        // Only a hint for non-seekable input; reads/materialization verify it.
        size_ = stat.cbSize.QuadPart;
        sizeKnown_ = true;
    }

    return true;
}

bool BoundedStreamSource::ReserveCache() noexcept
{
    const std::uint64_t bytes = kBlockBytes * kBlockSlots;
    auto reservation = ledger_->ReserveScoped(bytes);
    if (!reservation.has_value()) {
        return Fail(ProviderOutcome::LimitExceeded);
    }
    cacheReservation_ = std::move(*reservation);

    try {
        cache_.resize(kBlockSlots);
        for (Block& block : cache_) {
            block.bytes.resize(static_cast<std::size_t>(kBlockBytes));
        }
    } catch (const std::bad_alloc&) {
        return Fail(ProviderOutcome::OutOfMemory);
    }
    return true;
}

bool BoundedStreamSource::ReserveBacking(std::uint64_t total) noexcept
{
    if (total <= backingReserved_) {
        return true;
    }
    const std::uint64_t delta = total - backingReserved_;
    if (!ledger_->TryReserve(delta)) {
        return Fail(ProviderOutcome::LimitExceeded);
    }
    backingReserved_ = total;
    return true;
}

bool BoundedStreamSource::FillBlock(std::uint64_t index) noexcept
{
    if (!seekable_) {
        return Fail(ProviderOutcome::Unsupported);
    }
    if (!deadline_.Checkpoint()) {
        return Fail(ProviderOutcome::Deadline);
    }

    Block& block = cache_[static_cast<std::size_t>(index % kBlockSlots)];
    if (block.valid && block.index == index) {
        return true;
    }

    const std::uint64_t blockStart = index * kBlockBytes;
    if (blockStart >= ProviderLimits::kStreamMaxBytes) {
        return Fail(ProviderOutcome::LimitExceeded);
    }

    const std::uint64_t remaining =
        (sizeKnown_ && size_ > blockStart) ? size_ - blockStart : kBlockBytes;
    const std::size_t expected = static_cast<std::size_t>(
        (std::min)(remaining, kBlockBytes));
    if (expected == 0) {
        return Fail(ProviderOutcome::BadFormat);
    }

    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(blockStart);
    if (FAILED(stream_->Seek(position, STREAM_SEEK_SET, nullptr))) {
        return Fail(ProviderOutcome::Unsupported);
    }

    ULONG got = 0;
    const HRESULT hr = stream_->Read(block.bytes.data(),
                                     static_cast<ULONG>(expected), &got);
    if (FAILED(hr)) {
        return Fail(ProviderOutcome::BadFormat);
    }

    if (got != expected) {
        if (!sizeKnown_ && got <= expected) {
            // Unknown-size seekable stream: a short tail block pins the
            // validated size at EOF instead of being an inconsistency.
            size_ = blockStart + got;
            sizeKnown_ = true;
        } else {
            return Fail(ProviderOutcome::BadFormat);
        }
    }

    block.index = index;
    block.length = got;
    block.valid = true;
    return true;
}

bool BoundedStreamSource::ReadViaCache(std::uint64_t offset,
                                       std::span<std::byte> dest) noexcept
{
    if (!deadline_.Checkpoint()) {
        return Fail(ProviderOutcome::Deadline);
    }
    if (dest.empty()) {
        return true;
    }

    const std::uint64_t limit =
        sizeKnown_ ? size_ : ProviderLimits::kStreamMaxBytes;
    if (!FitsInRange(offset, dest.size(), limit)) {
        return Fail(ProviderOutcome::BadFormat);
    }

    std::size_t done = 0;
    while (done < dest.size()) {
        const std::uint64_t absolute = offset + done;
        const std::uint64_t index = absolute / kBlockBytes;
        const std::size_t within = static_cast<std::size_t>(absolute % kBlockBytes);
        if (!FillBlock(index)) {
            return false;
        }
        const Block& block = cache_[static_cast<std::size_t>(index % kBlockSlots)];
        if (within >= block.length) {
            // The block ended before the validated extent: a short read.
            return Fail(ProviderOutcome::BadFormat);
        }
        const std::size_t available = block.length - within;
        const std::size_t take =
            (std::min)(available, dest.size() - done);
        std::memcpy(dest.data() + done, block.bytes.data() + within, take);
        done += take;
    }
    return true;
}

bool BoundedStreamSource::Materialize() noexcept
{
    if (backingReady_) {
        return true;
    }

    const std::uint64_t cap = ProviderLimits::kContiguousBackingMaxBytes;

    if (seekable_) {
        if (!sizeKnown_) {
            // Random-access contiguity cannot be placed within the backing cap
            // without a validated extent.
            return Fail(ProviderOutcome::Unsupported);
        }
        if (size_ > cap) {
            return Fail(ProviderOutcome::LimitExceeded);
        }
        if (!ReserveBacking(size_)) {
            return false;
        }
        try {
            backing_.resize(static_cast<std::size_t>(size_));
        } catch (const std::bad_alloc&) {
            return Fail(ProviderOutcome::OutOfMemory);
        }
        if (size_ != 0 &&
            !ReadViaCache(0, std::span<std::byte>(backing_.data(), backing_.size()))) {
            return false;
        }
    } else {
        // Non-seekable input: read forward from the stream's current position
        // into the bounded backing buffer. Charge before each growth.
        for (;;) {
            if (backing_.size() >= cap) {
                return Fail(ProviderOutcome::LimitExceeded);
            }
            const std::size_t room = static_cast<std::size_t>(cap - backing_.size());
            const std::size_t want = static_cast<std::size_t>(
                (std::min)(kMaterializeChunkBytes,
                           static_cast<std::uint64_t>(room)));

            const std::uint64_t grown = static_cast<std::uint64_t>(backing_.size()) +
                                        static_cast<std::uint64_t>(want);
            const std::uint64_t doubled =
                static_cast<std::uint64_t>(backing_.capacity()) * 2u;
            const std::uint64_t newCap =
                (std::min)(cap, (std::max)(grown, doubled));
            if (!ReserveBacking(newCap)) {
                return false;
            }

            const std::size_t oldSize = backing_.size();
            try {
                backing_.reserve(static_cast<std::size_t>(newCap));
                backing_.resize(oldSize + want);
            } catch (const std::bad_alloc&) {
                return Fail(ProviderOutcome::OutOfMemory);
            }

            ULONG got = 0;
            const HRESULT hr = stream_->Read(backing_.data() + oldSize,
                                             static_cast<ULONG>(want), &got);
            if (FAILED(hr)) {
                return Fail(ProviderOutcome::BadFormat);
            }
            backing_.resize(oldSize + got);

            if (got < want) {
                break; // EOF
            }
            if (!deadline_.Checkpoint()) {
                return Fail(ProviderOutcome::Deadline);
            }
        }

        if (sizeKnown_ && size_ != backing_.size()) {
            return Fail(ProviderOutcome::BadFormat);
        }
        size_ = backing_.size();
        sizeKnown_ = true;
    }

    backingReady_ = true;
    return true;
}

std::uint64_t BoundedStreamSource::Size() const noexcept
{
    return sizeKnown_ ? size_ : 0;
}

bool BoundedStreamSource::Seekable() const noexcept { return seekable_; }

bool BoundedStreamSource::ReadAt(std::uint64_t offset,
                                 std::span<std::byte> dest)
{
    failure_ = ProviderOutcome::Success;
    if (dest.empty()) {
        return true;
    }

    if (seekable_) {
        return ReadViaCache(offset, dest);
    }

    if (!Materialize()) {
        return false;
    }
    if (!FitsInRange(offset, dest.size(), size_)) {
        return Fail(ProviderOutcome::BadFormat);
    }
    std::memcpy(dest.data(), backing_.data() + offset, dest.size());
    return true;
}

std::span<const std::byte> BoundedStreamSource::ContiguousView()
{
    failure_ = ProviderOutcome::Success;
    if (!Materialize()) {
        return {};
    }
    return std::span<const std::byte>(backing_.data(), backing_.size());
}

} // namespace preview3d::provider
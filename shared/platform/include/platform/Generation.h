#pragma once

// Product-owned generation/cancellation primitive -- a Gate 0 deliverable
// (.docs/design/10-delivery-plan.md: "cancellation/generation primitive")
// that was left unbuilt until Gate 2 workstream B slice 3 needed a real
// "is this still current" check for fence-complete publication (see
// .docs/design/04-rendering-and-streaming.md, "Fence-complete
// publication": "Graphics validates that its generation is still
// current"). A generation bump IS the cancellation signal -- there is no
// separate cancel flag/token type to set independently, matching how the
// design doc already uses a bare generationId everywhere (e.g.
// shared/model-core/include/model_core/ControlProtocol.h's
// StartGenerationRequest).

#include <atomic>
#include <cstdint>

namespace platform {

class GenerationToken;

// Thread-safe monotonic counter. Advance() starts a new generation, which
// implicitly invalidates every GenerationToken snapshotted from an
// earlier generation.
class GenerationSource {
public:
    GenerationSource() noexcept = default;
    GenerationSource(const GenerationSource&) = delete;
    GenerationSource& operator=(const GenerationSource&) = delete;

    uint64_t Current() const noexcept { return value_.load(std::memory_order_acquire); }

    // Returns the new value. Starts at 1 -- 0 is reserved as "never
    // valid," matching D3D12CommandQueue's fence-value convention
    // elsewhere in this repo.
    uint64_t Advance() noexcept { return value_.fetch_add(1, std::memory_order_acq_rel) + 1; }

    GenerationToken Snapshot() const noexcept;

private:
    std::atomic<uint64_t> value_{ 1 };
};

// An immutable snapshot of a generation value. Cheap to copy and holds no
// reference to its source -- IsCurrent() takes the source explicitly so a
// token can be carried across threads/queues independently of the
// source's own lifetime.
class GenerationToken {
public:
    GenerationToken() noexcept = default;
    explicit GenerationToken(uint64_t value) noexcept : value_(value) {}

    uint64_t Value() const noexcept { return value_; }
    bool IsCurrent(const GenerationSource& source) const noexcept { return value_ == source.Current(); }

private:
    uint64_t value_ = 0;
};

inline GenerationToken GenerationSource::Snapshot() const noexcept
{
    return GenerationToken(Current());
}

} // namespace platform

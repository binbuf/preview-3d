#pragma once

// T06 provider budgets and checked arithmetic.
//
// This header is the single checked source of truth for the accountable limits
// in design/03-file-formats-and-ingestion.md (*Thumbnail host* column) and
// design/05-thumbnail-provider.md (stream/backing/scratch caps, process-commit
// target, sampled-geometry and per-component decode caps). Every adapter
// (T21-T34), the sampler (T14), the rasterizer (T15) and the bounded source
// (T12) consume these names instead of growing a private limit.
//
// Limits are acceptance ceilings, not a guarantee that a limit-sized scene fits
// at once. Above-limit accountable input fails to the generic icon with the
// first exceeded limit named (ClassifyError/HresultFor in ProviderErrors.h).
//
// The 384 MiB figures are two different things and must not be conflated:
//   - kAllocationLedgerMaxBytes is the ceiling the product-owned allocation
//     ledger enforces (AllocationLedger.h), summing controlled charges across
//     concurrent calls;
//   - kProcessCommitQualificationTargetBytes is a measured release target for
//     total process private commit above the idle, loaded surrogate baseline.
//     It cannot be enforced against library allocations without callbacks, DLL
//     loading or GDI/Shell overhead, so T51 measures the actual peak.
//
// This header deliberately reuses the shared checked primitives in
// shared/platform/include/platform/CheckedMath.h (compiled into the provider,
// design/interfaces.md) and adds the file-derived range helpers used at every
// allocation/offset.

#include "platform/CheckedMath.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace preview3d::provider {

// --- Checked integer/range helpers -----------------------------------------

// Addition that reports overflow instead of wrapping.
constexpr std::optional<std::uint64_t> CheckedAdd(std::uint64_t a,
                                                  std::uint64_t b) noexcept
{
    return platform::CheckedAdd(a, b);
}

// Multiplication that reports overflow instead of wrapping.
constexpr std::optional<std::uint64_t> CheckedMultiply(std::uint64_t a,
                                                       std::uint64_t b) noexcept
{
    return platform::CheckedMultiply(a, b);
}

// End offset of [offset, offset + size), or nullopt when the addition
// overflows. A zero size yields the offset itself.
constexpr std::optional<std::uint64_t> CheckedRangeEnd(std::uint64_t offset,
                                                       std::uint64_t size) noexcept
{
    return platform::CheckedAdd(offset, size);
}

// True only when [offset, offset + size) lies completely inside [0, limit] and
// the addition did not overflow. Every file-derived offset/size pair is checked
// with this before it is used to index or allocate.
constexpr bool FitsInRange(std::uint64_t offset, std::uint64_t size,
                           std::uint64_t limit) noexcept
{
    const auto end = platform::CheckedAdd(offset, size);
    return end.has_value() && *end <= limit;
}

// Checked narrowing of a file-derived value into a fixed-width integer type.
// Returns nullopt when the value does not fit (including a negative signed
// destination cannot represent).
template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
constexpr std::optional<T> CheckedNarrow(std::uint64_t value) noexcept
{
    if (value > static_cast<std::uint64_t>((std::numeric_limits<T>::max)())) {
        return std::nullopt;
    }
    return static_cast<T>(value);
}

// --- Frozen provider budgets ------------------------------------------------

// `ProviderTypes.h` forward-declares this name; T06 defines it here with the
// frozen caps. The caps are `static constexpr` so adapters and tests compare
// them directly, and `Default()` hands the frozen set to the `const
// ProviderLimits*` carried by `AdapterInput`/`RasterRequest`.
struct ProviderLimits {
    // Maximum source data ever read or cached from the Shell stream. Not a
    // product-wide file-size maximum: an oversized file simply gets the generic
    // icon after a cheap size check.
    static constexpr std::uint64_t kStreamMaxBytes = 256ull * 1024 * 1024;

    // Largest single contiguous in-process backing buffer an adapter may ask
    // for (design/05, "Stream ingestion").
    static constexpr std::uint64_t kContiguousBackingMaxBytes = 128ull * 1024 * 1024;

    // Parser/normalizer live scratch the provider accounts for.
    static constexpr std::uint64_t kAccountedScratchMaxBytes = 192ull * 1024 * 1024;

    // Ceiling enforced by AllocationLedger for product-owned charges across
    // concurrent GetThumbnail calls.
    static constexpr std::uint64_t kAllocationLedgerMaxBytes = 384ull * 1024 * 1024;

    // Measured release target for total process private commit above the idle,
    // loaded surrogate baseline (T51); not enforced by the ledger.
    static constexpr std::uint64_t kProcessCommitQualificationTargetBytes =
        384ull * 1024 * 1024;

    // Geometry inspection caps (design/05, "Geometry sampling").
    static constexpr std::uint64_t kTrianglesInspectedMax = 2'000'000;
    static constexpr std::uint64_t kPointsInspectedMax = 6'000'000;

    // Representative samples admitted into rasterization.
    static constexpr std::uint64_t kRasterizedSamplesMax = 250'000;

    // Aggregate decoded texture pixels. Decimal megapixels to match the
    // viewer's gigapixel/megapixel budgets.
    static constexpr std::uint64_t kDecodedTexturePixelsMax = 32ull * 1'000'000;

    // Scene-graph caps.
    static constexpr std::uint32_t kNodesMax = 10'000;
    static constexpr std::uint32_t kMaterialsMax = 4'096;

    // One Draco primitive decoded working set (design/03, "One Draco primitive
    // decoded working set": 96 MiB and 1 million triangles for the thumbnail
    // host).
    static constexpr std::uint64_t kDracoDecodedWorkingSetMaxBytes =
        96ull * 1024 * 1024;
    static constexpr std::uint64_t kDracoTrianglesMax = 1'000'000;

    // The frozen set handed to adapters through the `const ProviderLimits*` in
    // `AdapterInput`/`RasterRequest`. Stable for the process lifetime.
    static const ProviderLimits& Default() noexcept
    {
        static const ProviderLimits limits{};
        return limits;
    }
};

} // namespace preview3d::provider
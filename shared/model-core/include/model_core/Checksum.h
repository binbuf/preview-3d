#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace model_core {

inline uint64_t Fnv1a64Append(uint64_t hash, std::span<const std::byte> data) noexcept
{
    constexpr uint64_t kPrime = 1099511628211ULL;
    for (std::byte b : data) {
        hash ^= static_cast<uint64_t>(b);
        hash *= kPrime;
    }
    return hash;
}

// FNV-1a 64-bit. Hand-written because vcpkg.json pins only catch2 and no
// checksum library is otherwise available. Deliberately non-cryptographic:
// a worker that computes its own checksum can forge any checksum, so no
// checksum algorithm stops a lying worker -- the real defense is the
// copy-then-validate bounds/arithmetic checks in
// shared/import-broker/SharedSectionValidator.h, proven adversarially by the
// (deferred) synthetic hostile-worker suite. This only catches incidental
// corruption on the honest-worker path.
inline uint64_t Fnv1a64(std::span<const std::byte> data) noexcept
{
    constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
    return Fnv1a64Append(kOffsetBasis, data);
}

} // namespace model_core

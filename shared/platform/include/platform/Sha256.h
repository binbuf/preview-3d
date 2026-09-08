#pragma once

// SHA-256 via Windows CNG (BCrypt) -- no new vcpkg dependency; bcrypt.h/
// bcrypt.lib ship with the OS. Built for DerivedCache's per-section
// integrity validation (.docs/design/04-rendering-and-streaming.md,
// "Persistent derived-data cache": "per-section SHA-256"). The only hash
// primitive in this repo before this was model_core::Fnv1a64, whose own
// header comment explicitly disclaims cryptographic use.

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace platform {

// Returns nullopt only if the OS-provided algorithm provider itself fails
// to open/hash -- not expected in practice on any supported Windows
// version, since BCRYPT_SHA256_ALGORITHM is a mandatory inbox provider.
std::optional<std::array<std::byte, 32>> ComputeSha256(std::span<const std::byte> data);

} // namespace platform

#pragma once

#include "model_core/ImportError.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <variant>
#include <vector>

namespace import_worker {

enum class MeshoptDecodeMode { Attributes, Triangles, Indices };
enum class MeshoptDecodeFilter { None, Octahedral, Quaternion, Exponential, Color };

struct MeshoptDecodeOptions {
    uint64_t maxDecodedBytes = 512ull * 1024 * 1024;
    std::function<bool()> isCancelled;
};

// Decodes exactly one EXT_meshopt_compression bufferView. The caller owns
// aggregate/source accounting; this boundary owns the independent decode-unit
// cap and validates every shape accepted by meshoptimizer before calling it.
std::variant<std::vector<std::byte>, model_core::ImportErrorCode> DecodeMeshoptBuffer(
    std::span<const std::byte> encoded, uint64_t count, uint64_t stride,
    uint64_t decodedByteLength, MeshoptDecodeMode mode, MeshoptDecodeFilter filter,
    const MeshoptDecodeOptions& options = {});

} // namespace import_worker

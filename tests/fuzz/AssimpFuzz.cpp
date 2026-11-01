#ifndef NOMINMAX
#define NOMINMAX
#endif

// Standalone sanitizer target for the T10 bounded Assimp compatibility
// adapter. A one-byte envelope selects one of the seven closed format hints;
// every other byte is the primary source. The adapter runs with the same
// in-process object/material/triangle/vertex ceilings as production, a small
// in-memory destination window and a sidecar callback that denies every
// external reference, so this exercises Assimp container parsing, scene
// validation, material conversion and normalized chunk emission without any
// filesystem, broker, GPU or child process.
//
// The compressed texture decoder is intentionally a stub here: codec coverage
// belongs to the dedicated codec targets and is not the compatibility
// container's attack surface.

#include "AssimpAdapter.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

constexpr size_t kInputLimit = 2u * 1024u * 1024u;
constexpr size_t kDestinationBytes = 4u * 1024u * 1024u;

import_worker::AssimpSidecarResult DenySidecar(std::string_view reference, uint64_t)
{
    import_worker::AssimpSidecarResult result;
    result.error = reference.find("..") != std::string_view::npos
        || reference.find(':') != std::string_view::npos
        ? model_core::ImportErrorCode::UnsafeReference
        : model_core::ImportErrorCode::FileUnavailable;
    return result;
}

void FuzzOne(std::span<const std::byte> primary, model_core::AssimpFormat format)
{
    if (primary.empty() || primary.size() > kInputLimit) return;
    static std::vector<std::byte> destination;
    destination.resize(kDestinationBytes);

    import_worker::AssimpImportOptions options;
    options.format = format;
    options.requestSidecar = DenySidecar;
    options.maxObjects = 1024;
    options.maxMaterials = 1024;
    options.maxTriangles = 100'000;
    options.maxVertices = 300'000;
    options.maxSidecarRequests = 8;
    options.textureOptions.maxEncodedBytes = 1u * 1024u * 1024u;
    options.textureOptions.maxDecodedBytes = 4u * 1024u * 1024u;
    options.textureOptions.maxPixels = 4u * 1024u * 1024u;
    options.textureOptions.maxDimension = 1024;

    (void)import_worker::ImportAssimp(primary, destination, 1, 1024, nullptr, options);
}

} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (!data || !size || size > kInputLimit + 1) return 0;
    const auto format = model_core::AssimpFormat(data[0] % 7u);
    if (!model_core::IsKnownAssimpFormat(format)) return 0;
    const std::span<const std::byte> primary(
        reinterpret_cast<const std::byte*>(data + 1), size - 1);
    FuzzOne(primary, format);
    return 0;
}

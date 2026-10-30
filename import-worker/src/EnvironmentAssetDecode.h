#pragma once

// Environment-asset hook for the Studio IBL work (T14).
//
// This is deliberately NOT a Material image chunk: it returns a linear
// equirectangular RGBA-float buffer in the internal environment representation.
// It takes bytes, never a path or URI, so the viewer's untrusted-content rules
// cannot reach it directly; callers must already hold a product-owned or
// broker-approved asset. Radiance HDR and single-part scanline OpenEXR are
// accepted, using the same bounded decoders and budgets as material textures.
//
// The product-owned procedural Studio environment is T14's preferred source and
// needs no decoding. This hook exists for an embedded HDRI alternative; it must
// not be linked into the viewer (the decoders stay in the import worker).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace import_worker {

struct EnvironmentAssetOptions {
    uint64_t maxEncodedBytes = 64ull * 1024 * 1024;
    // Float RGBA storage after decode/downscale.
    uint64_t maxDecodedBytes = 256ull * 1024 * 1024;
    uint64_t maxPixels = 64ull * 1024 * 1024;
    uint32_t maxDimension = 4096;
    std::function<bool()> isCancelled;
    bool Cancelled() const { return isCancelled && isCancelled(); }
};

struct EnvironmentAsset {
    uint32_t width = 0;  // equirectangular, top-left origin
    uint32_t height = 0;
    std::vector<float> rgba;  // tightly packed linear RGBA, 4 floats per texel

    bool Valid() const
    {
        return width != 0 && height != 0
            && rgba.size() == size_t(width) * height * 4;
    }
};

// Sniffs HDR/EXR from the bytes and returns a bounded linear equirect image.
// Returns nullopt for an unknown/corrupt/over-budget/cancelled asset.
std::optional<EnvironmentAsset> LoadEnvironmentAsset(std::span<const std::byte> bytes,
                                                     const EnvironmentAssetOptions& options = {});

} // namespace import_worker

#pragma once

// Product-owned, bounded Radiance RGBE (.hdr) decoder.
//
// Supported subset:
//  - `#?RADIANCE` or `#?RGBE` signature;
//  - one `FORMAT=32-bit_rle_rgbe` header (XYZ E files are rejected);
//  - the conventional `-Y <height> +X <width>` resolution line. Both signs are
//    accepted for X/Y so flipped strips decode, but the Y-then-X axis order is
//    required;
//  - new-style per-channel RLE scanlines (width 8..32767) and flat RGBE
//    scanlines. Old-style `0x01 0x01 0x01` RLE is not implemented.
//
// The decoder produces scene-linear float data. For material texture slots it
// uses the documented HDR policy in HdrFloatDecode.h (Reinhard tone map for
// color/emissive, clamp-only for data/normal). Malformed or over-budget files
// soft-fail to std::nullopt so an optional texture degrades to a warning.

#include "HdrFloatDecode.h"
#include "TextureTranscodeAdapter.h"

#include <cstddef>
#include <optional>
#include <span>

namespace import_worker {

std::optional<TranscodedImage> DecodeRadianceHdrImage(std::span<const std::byte> bytes,
                                                      model_core::ColorSpaceId colorSpace,
                                                      const TextureDecodeOptions& options = {});

// Environment hook path: linear RGBA float, no material tonemap.
std::optional<LinearRgbaImage> DecodeRadianceHdrToLinear(
    std::span<const std::byte> bytes, const TextureDecodeOptions& options = {});

} // namespace import_worker

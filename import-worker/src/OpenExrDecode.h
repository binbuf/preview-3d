#pragma once

// Bounded single-part scanline OpenEXR decoder built on the tinyexr copy that
// already ships inside the pinned TinyUSDZ closure (no new vcpkg port and no
// decoder in the viewer).
//
// Supported subset:
//  - single-part, non-tiled, non-deep scanline images (version flags tiled,
//    non_image/deep and multipart are rejected before the header is used);
//  - compression NONE, RLE, ZIPS, ZIP and PIZ (the tinyexr-supported set);
//    B44/B44A/DWAA/DWAB/ZFP and unknown values are rejected explicitly;
//  - HALF or FLOAT channels with x/y sampling 1. UINT channels are rejected;
//  - R/G/B/A or Y channel names, including layered `layer.R` names, with a
//    fallback to the first three channels (or the single channel as grey).
//
// The product code validates the data window, channel count, pixel budget and
// transient float allocation before tinyexr allocates anything, and requests
// FLOAT output so half and float files share one code path. The loaded image is
// converted to the same linear RGBA float representation as HDR and then goes
// through HdrFloatDecode.h's documented material policy.

#include "HdrFloatDecode.h"
#include "TextureTranscodeAdapter.h"

#include <cstddef>
#include <optional>
#include <span>

namespace import_worker {

std::optional<TranscodedImage> DecodeOpenExrImage(std::span<const std::byte> bytes,
                                                  model_core::ColorSpaceId colorSpace,
                                                  const TextureDecodeOptions& options = {});

// Environment hook path: linear RGBA float, no material tonemap.
std::optional<LinearRgbaImage> DecodeOpenExrToLinear(std::span<const std::byte> bytes,
                                                     const TextureDecodeOptions& options = {});

} // namespace import_worker

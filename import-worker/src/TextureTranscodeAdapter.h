#pragma once

// KTX2/Basis Universal texture transcode for a KHR_texture_basisu image,
// using the real KTX-Software library. Deliberately soft-failing: any
// problem here (corrupt container, transcode failure, unsupported dialect)
// returns std::nullopt, never an ImportErrorCode -- the caller (GltfAdapter)
// treats this as "material has no base color texture," never a hard import
// failure. Draco geometry decode is the asymmetric opposite: required
// geometry fails hard. See DracoDecodeAdapter.h and R-19 in
// .docs/design/11-decisions-and-risks.md.
//
// Scope this slice: base color only, transcoded to a single fixed target
// (BC7, RGBA8 fallback) matching
// .docs/design/03-file-formats-and-ingestion.md's texture policy verbatim
// ("Basis payloads transcode to a supported BC7/BC5/BC3/BC1 target selected
// by semantic and adapter capabilities, with RGBA8 fallback") -- semantic-
// based BC5/BC3/BC1 selection is deferred to a later slice that decodes
// normal/metallicRoughness maps. Mip chain narrowed to level 0 only
// (mipLevels always 1 in the result); colorSpace is always sRGB (base color
// is the only semantic handled here). Both are adapter-scope narrowings,
// not model_core::PixelFormats.h wire-format limitations.

#include "model_core/PixelFormats.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace import_worker {

struct TranscodedImage {
    model_core::PixelFormatId pixelFormat = model_core::PixelFormatId::Unknown; // BC7_UNORM or RGBA8_UNORM
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<std::byte> pixelBytes; // level 0 only, tightly packed per PixelFormats.h's layout
};

// ktx2Bytes: raw bytes of a KTX2 container (GLB-embedded, per
// KHR_texture_basisu). Returns nullopt on any failure -- never throws past
// this boundary.
std::optional<TranscodedImage> TranscodeKtx2BasisImage(std::span<const std::byte> ktx2Bytes);

} // namespace import_worker

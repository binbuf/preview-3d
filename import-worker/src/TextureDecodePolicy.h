#pragma once

#include "model_core/PixelFormats.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <span>
#include <vector>

namespace import_worker {

enum class TextureSemantic { Color, Data, Normal, Emissive };

struct TextureDecodeOptions {
    // Aggregate encoded (compressed source) bytes read for one generation's
    // textures. 16-bit 4K PNG maps are commonly tens of MiB each, so this has
    // to comfortably hold a full set; the decoded-pixel budget is the memory
    // gate, not this transient read budget.
    uint64_t maxEncodedBytes = 512ull * 1024 * 1024;
    uint64_t maxDecodedBytes = 32ull * 1024 * 1024;
    uint64_t maxPixels = 1'000'000'000;
    uint32_t maxDimension = 2048;
    TextureSemantic semantic = TextureSemantic::Color;
    std::function<bool()> isCancelled;
    bool Cancelled() const { return isCancelled && isCancelled(); }
};

// In-place tightly packed RGBA chain. Each output tile is at most 64x64;
// cancellation is checked before allocation and between tiles, never per image.
inline bool GenerateRasterMips(std::vector<std::byte>& pixels, uint32_t width, uint32_t height,
                               model_core::ColorSpaceId space, const TextureDecodeOptions& options,
                               uint32_t& levels)
{
    levels = model_core::FullImageMipCount(width, height);
    const auto bytes = model_core::ComputeImagePixelBytes(model_core::PixelFormatId::RGBA8_UNORM, width, height, levels);
    const auto base = model_core::ComputeImagePixelBytes(model_core::PixelFormatId::RGBA8_UNORM, width, height, 1);
    if (options.Cancelled() || !bytes || !base || pixels.size() != *base || *bytes > options.maxDecodedBytes) return false;
    pixels.resize(static_cast<size_t>(*bytes));
    size_t sourceOffset = 0, targetOffset = static_cast<size_t>(*base);
    for (uint32_t level = 1; level < levels; ++level) {
        const uint32_t nextW = (std::max)(1u, width/2), nextH = (std::max)(1u, height/2);
        for (uint32_t tileY = 0; tileY < nextH; tileY += 64) {
            for (uint32_t tileX = 0; tileX < nextW; tileX += 64) {
                if (options.Cancelled()) return false;
                for (uint32_t y = tileY; y < (std::min)(nextH, tileY+64); ++y) {
                    for (uint32_t x = tileX; x < (std::min)(nextW, tileX+64); ++x) {
                        float sum[4]{};
                        // Area partition includes the trailing row/column of odd images.
                        const uint32_t x0 = x*width/nextW, x1 = (x+1)*width/nextW;
                        const uint32_t y0 = y*height/nextH, y1 = (y+1)*height/nextH;
                        for (uint32_t sy=y0; sy<y1; ++sy) for (uint32_t sx=x0; sx<x1; ++sx) {
                            for (unsigned c=0; c<4; ++c) {
                                float v = float(std::to_integer<uint8_t>(pixels[sourceOffset+(size_t(sy)*width+sx)*4+c]))/255.0f;
                                if (space == model_core::ColorSpaceId::Srgb && c<3)
                                    v = v <= 0.04045f ? v/12.92f : std::pow((v+0.055f)/1.055f,2.4f);
                                sum[c] += v;
                            }
                        }
                        for (float& v : sum) v /= float((x1-x0)*(y1-y0));
                        if (options.semantic == TextureSemantic::Normal) {
                            float n[3]{sum[0]*2-1,sum[1]*2-1,sum[2]*2-1};
                            const float length = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
                            for (unsigned c=0;c<3;++c) sum[c] = length>1e-6f ? n[c]/length*0.5f+0.5f : (c==2 ? 1.0f : 0.5f);
                        }
                        for (unsigned c=0;c<4;++c) {
                            float v=sum[c];
                            if (space == model_core::ColorSpaceId::Srgb && c<3)
                                v = v<=0.0031308f ? v*12.92f : 1.055f*std::pow(v,1.0f/2.4f)-0.055f;
                            pixels[targetOffset+(size_t(y)*nextW+x)*4+c] = std::byte(uint8_t(std::clamp(std::lround(v*255),0l,255l)));
                        }
                    }
                }
            }
        }
        sourceOffset=targetOffset; targetOffset += size_t(nextW)*nextH*4;
        width=nextW; height=nextH;
    }
    return !options.Cancelled();
}

} // namespace import_worker

#pragma once

// T17 golden-image comparator shared by Tests.ProviderHost.exe and, by copy of
// this header's contract, every family task that registers a fixture.
//
// docs/design/testing-strategy.md ("Golden-image policy") fixes the metric:
// mean absolute error per channel plus a max-outlier guard, not byte equality,
// so platform/CPU floating-point rounding is tolerated while a real regression
// is caught. The rasterizer returns premultiplied BGRA and the committed PAM
// goldens store premultiplied RGBA, so the comparator swaps channels the same
// way T15's rasterizer coverage does.
//
// A golden is loaded through `thumbnail_rasterizer::LoadPamRgba` (the same
// dependency-free PAM container the T15 goldens use) and written with
// `thumbnail_rasterizer::SaveBgraAsPam`. Family tasks add a fixture to
// `FixtureRegistry.cpp`; the hidden `[write-host-goldens]` case regenerates the
// committed PAMs exactly as `[write-goldens]` does for the rasterizer suite.

#include "CpuRasterizer.h"

#include "PamImage.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace preview3d::test {

// The tolerance a fixture is compared with. Defaults match the T15 rasterizer
// suite so a host golden and a direct rasterizer golden are judged the same.
struct GoldenTolerance {
    double meanAbs = 2.0;
    int maxAbs = 48;
};

struct GoldenDiff {
    bool valid = false;      // both images had consistent dimensions/sizes
    std::size_t pixels = 0;  // compared pixel count
    double meanAbs = 0.0;    // mean absolute error over every channel
    int maxAbs = 0;          // worst single-channel outlier
    int width = 0;
    int height = 0;
};

// Compares a rendered premultiplied BGRA image with a premultiplied RGBA golden.
// Returns `valid == false` when the two are not the same size.
inline GoldenDiff CompareBgraToRgba(const std::vector<std::uint8_t>& bgra, int width, int height,
                                    const std::vector<std::uint8_t>& rgba) noexcept
{
    GoldenDiff diff;
    diff.width = width;
    diff.height = height;
    if (width <= 0 || height <= 0) {
        return diff;
    }
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    diff.pixels = pixels;
    if (bgra.size() != pixels * 4 || rgba.size() != pixels * 4) {
        return diff;
    }
    diff.valid = true;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t rendered[4] = {bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4 + 0],
                                          bgra[i * 4 + 3]};
        for (int c = 0; c < 4; ++c) {
            const int delta = std::abs(static_cast<int>(rendered[c]) -
                                       static_cast<int>(rgba[i * 4 + c]));
            total += static_cast<std::uint64_t>(delta);
            diff.maxAbs = (std::max)(diff.maxAbs, delta);
        }
    }
    diff.meanAbs = static_cast<double>(total) / static_cast<double>(pixels * 4);
    return diff;
}

// True only when a valid comparison is inside both the mean and outlier bounds.
inline bool WithinTolerance(const GoldenDiff& diff, const GoldenTolerance& tolerance) noexcept
{
    return diff.valid && diff.meanAbs <= tolerance.meanAbs && diff.maxAbs <= tolerance.maxAbs;
}

inline bool LoadGoldenRgba(const std::string& path, int& width, int& height,
                           std::vector<std::uint8_t>& rgba)
{
    return thumbnail_rasterizer::LoadPamRgba(path.c_str(), width, height, rgba);
}

inline bool SaveGolden(const std::string& path, const preview3d::provider::RasterImage& image)
{
    if (image.width == 0 || image.height == 0) {
        return false;
    }
    return thumbnail_rasterizer::SaveBgraAsPam(path.c_str(), static_cast<int>(image.width),
                                               static_cast<int>(image.height),
                                               image.bgraPremultiplied);
}

} // namespace preview3d::test
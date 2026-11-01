#include "OpenExrDecode.h"

// Pinned copy of the tinyexr single-header decoder (BSD-3-Clause) taken from
// the vendored TinyUSDZ closure. Building the implementation in this TU keeps
// the decoder out of the viewer and avoids depending on tinyusdz's internal
// archive layout. System zlib is already a pinned product dependency.
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 0
#define TINYEXR_USE_NANOZLIB 0
#define TINYEXR_USE_PIZ 1
#define TINYEXR_USE_OPENMP 0
#include <zlib.h>
#define TINYEXR_IMPLEMENTATION
#pragma warning(push)
// Pinned third-party header; keep its warnings from failing /WX builds.
#pragma warning(disable : 4100 4245 4267 4702 4706 4996)
#include "../../third_party/tinyexr/tinyexr.h"
#pragma warning(pop)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

namespace import_worker {

namespace {

// tinyexr's parse/load entry points own their partial cleanup on failure; only
// call Free* after a successful return to avoid a double free on hostile input.
struct ExrHeaderGuard {
    EXRHeader header{};
    bool owned = false;
    ExrHeaderGuard() { InitEXRHeader(&header); }
    ~ExrHeaderGuard()
    {
        if (owned) FreeEXRHeader(&header);
    }
    ExrHeaderGuard(const ExrHeaderGuard&) = delete;
    ExrHeaderGuard& operator=(const ExrHeaderGuard&) = delete;
};

struct ExrImageGuard {
    EXRImage image{};
    bool owned = false;
    ExrImageGuard() { InitEXRImage(&image); }
    ~ExrImageGuard()
    {
        if (owned) FreeEXRImage(&image);
    }
    ExrImageGuard(const ExrImageGuard&) = delete;
    ExrImageGuard& operator=(const ExrImageGuard&) = delete;
};

bool EqualsIgnoreCase(const char* text, const char* wanted) noexcept
{
    if (!text || !wanted) return false;
    while (*text && *wanted) {
        const unsigned char left = static_cast<unsigned char>(*text++);
        const unsigned char right = static_cast<unsigned char>(*wanted++);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return *text == '\0' && *wanted == '\0';
}

// EXR channel names are often prefixed by a layer ("beauty.R"). Match the last
// dot-separated component so layered files work without accepting arbitrary
// suffixes.
bool ChannelNameMatches(const char* name, const char* wanted) noexcept
{
    if (!name) return false;
    const char* leaf = name;
    for (const char* p = name; *p; ++p) {
        if (*p == '.') leaf = p + 1;
    }
    return EqualsIgnoreCase(leaf, wanted);
}

int FindChannel(const EXRHeader& header, const char* name) noexcept
{
    for (int i = 0; i < header.num_channels; ++i) {
        if (ChannelNameMatches(header.channels[i].name, name)) return i;
    }
    return -1;
}

bool CompressionSupported(int compression) noexcept
{
    switch (compression) {
    case TINYEXR_COMPRESSIONTYPE_NONE:
    case TINYEXR_COMPRESSIONTYPE_RLE:
    case TINYEXR_COMPRESSIONTYPE_ZIPS:
    case TINYEXR_COMPRESSIONTYPE_ZIP:
    case TINYEXR_COMPRESSIONTYPE_PIZ:
        return true;
    default:
        return false;
    }
}

struct ChannelPlan {
    int red = -1;
    int green = -1;
    int blue = -1;
    int alpha = -1;
};

std::optional<ChannelPlan> MapChannels(const EXRHeader& header)
{
    if (header.num_channels <= 0 || header.num_channels > 16) return std::nullopt;
    const int red = FindChannel(header, "R");
    const int green = FindChannel(header, "G");
    const int blue = FindChannel(header, "B");
    const int alpha = FindChannel(header, "A");
    const int luminance = FindChannel(header, "Y");
    ChannelPlan plan;
    plan.alpha = alpha;
    if (red >= 0 && green >= 0 && blue >= 0) {
        plan.red = red;
        plan.green = green;
        plan.blue = blue;
        return plan;
    }
    if (luminance >= 0) {
        plan.red = plan.green = plan.blue = luminance;
        return plan;
    }
    if (header.num_channels >= 3) {
        plan.red = 0;
        plan.green = 1;
        plan.blue = 2;
        return plan;
    }
    if (header.num_channels == 1) {
        plan.red = plan.green = plan.blue = 0;
        return plan;
    }
    return std::nullopt;
}

std::optional<LinearRgbaImage> DecodeLinear(std::span<const std::byte> bytes,
                                            const TextureDecodeOptions& options)
{
    if (options.Cancelled() || bytes.size() < 8 || bytes.size() > options.maxEncodedBytes) {
        return std::nullopt;
    }
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());

    EXRVersion version{};
    if (ParseEXRVersionFromMemory(&version, data, bytes.size()) != TINYEXR_SUCCESS) {
        return std::nullopt;
    }
    if (version.version != 2 || version.tiled != 0 || version.non_image != 0
        || version.multipart != 0) {
        return std::nullopt;
    }

    ExrHeaderGuard header_guard;
    const char* error = nullptr;
    if (ParseEXRHeaderFromMemory(&header_guard.header, &version, data, bytes.size(), &error)
        != TINYEXR_SUCCESS) {
        if (error) FreeEXRErrorMessage(error);
        return std::nullopt;
    }
    header_guard.owned = true;
    if (error) FreeEXRErrorMessage(error);
    const EXRHeader& header = header_guard.header;
    if (header.tiled != 0 || header.non_image != 0 || header.multipart != 0
        || !CompressionSupported(header.compression_type)) {
        return std::nullopt;
    }
    if (header.data_window.min_x > header.data_window.max_x
        || header.data_window.min_y > header.data_window.max_y) {
        return std::nullopt;
    }
    const int64_t width64 = int64_t(header.data_window.max_x) - header.data_window.min_x + 1;
    const int64_t height64 = int64_t(header.data_window.max_y) - header.data_window.min_y + 1;
    if (width64 <= 0 || height64 <= 0 || width64 > model_core::kMaxTextureDimension
        || height64 > model_core::kMaxTextureDimension) {
        return std::nullopt;
    }
    const uint64_t width = uint64_t(width64);
    const uint64_t height = uint64_t(height64);
    const uint64_t pixelCount = width * height;
    if (pixelCount == 0 || pixelCount > options.maxPixels
        || pixelCount * 16 > LinearFloatTransientLimit(options)) {
        return std::nullopt;
    }
    auto plan = MapChannels(header);
    if (!plan) return std::nullopt;
    for (int i = 0; i < header.num_channels; ++i) {
        if ((header.channels[i].x_sampling != 1 || header.channels[i].y_sampling != 1)
            || (header.pixel_types[i] != TINYEXR_PIXELTYPE_HALF
                && header.pixel_types[i] != TINYEXR_PIXELTYPE_FLOAT)) {
            return std::nullopt;
        }
        // Request float for every channel; tinyexr converts HALF on load.
        header.requested_pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT;
    }

    ExrImageGuard image_guard;
    if (LoadEXRImageFromMemory(&image_guard.image, &header, data, bytes.size(), &error)
        != TINYEXR_SUCCESS) {
        if (error) FreeEXRErrorMessage(error);
        return std::nullopt;
    }
    image_guard.owned = true;
    if (error) FreeEXRErrorMessage(error);
    const EXRImage& source = image_guard.image;
    if (!source.images || source.num_channels != header.num_channels
        || source.width != int(width) || source.height != int(height)) {
        return std::nullopt;
    }

    LinearRgbaImage image;
    image.width = uint32_t(width);
    image.height = uint32_t(height);
    image.rgba.assign(size_t(pixelCount) * 4, 0.0f);
    const auto* red = reinterpret_cast<const float*>(source.images[plan->red]);
    const auto* green = reinterpret_cast<const float*>(source.images[plan->green]);
    const auto* blue = reinterpret_cast<const float*>(source.images[plan->blue]);
    const auto* alpha = plan->alpha >= 0
        ? reinterpret_cast<const float*>(source.images[plan->alpha]) : nullptr;
    if (!red || !green || !blue) return std::nullopt;

    for (uint64_t i = 0; i < pixelCount; ++i) {
        if (options.Cancelled()) return std::nullopt;
        float* target = image.rgba.data() + size_t(i) * 4;
        const float values[3] = { red[i], green[i], blue[i] };
        for (unsigned c = 0; c < 3; ++c) {
            target[c] = std::isfinite(values[c]) ? values[c] : 0.0f;
        }
        target[3] = alpha ? (std::isfinite(alpha[i]) ? alpha[i] : 1.0f) : 1.0f;
    }
    return image;
}

} // namespace

std::optional<LinearRgbaImage> DecodeOpenExrToLinear(std::span<const std::byte> bytes,
                                                     const TextureDecodeOptions& options)
{
    return DecodeLinear(bytes, options);
}

std::optional<TranscodedImage> DecodeOpenExrImage(std::span<const std::byte> bytes,
                                                  model_core::ColorSpaceId colorSpace,
                                                  const TextureDecodeOptions& options)
{
    auto linear = DecodeLinear(bytes, options);
    if (!linear) return std::nullopt;
    return FinalizeLinearImage(std::move(*linear), colorSpace, options);
}

} // namespace import_worker

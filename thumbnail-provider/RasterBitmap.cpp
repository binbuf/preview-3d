// T13 HBITMAP boundary implementation (see RasterBitmap.h).
//
// Free of the provider precompiled header and of COM so the same translation unit
// compiles into Preview3DThumbnailProvider.dll and Tests.Unit.exe.

#include "RasterBitmap.h"

#include "ProviderLimits.h"

#include <cstdint>
#include <cstring>

namespace preview3d::provider {

ProviderOutcome CreatePremultipliedDib(const RasterImage& image, HBITMAP& out) noexcept
{
    out = nullptr;

    if (image.width == 0 || image.height == 0 ||
        image.width > static_cast<std::uint32_t>(INT32_MAX) ||
        image.height > static_cast<std::uint32_t>(INT32_MAX)) {
        return ProviderOutcome::BadFormat;
    }
    const auto expected = CheckedMultiply(image.width, image.height);
    if (!expected.has_value()) {
        return ProviderOutcome::BadFormat;
    }
    const auto expectedBytes = CheckedMultiply(*expected, 4u);
    if (!expectedBytes.has_value() || *expectedBytes != image.bgraPremultiplied.size()) {
        return ProviderOutcome::BadFormat;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(image.width);
    // A negative height selects a top-down DIB, matching the rasterizer's
    // row order (design/05, "CPU renderer").
    info.bmiHeader.biHeight = -static_cast<LONG>(image.height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    // No HDC is needed for a DIB_RGB_COLORS section, so no other GDI object is
    // created or must be released.
    HBITMAP bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        if (bitmap != nullptr) {
            ::DeleteObject(bitmap);
        }
        return ProviderOutcome::OutOfMemory;
    }

    std::memcpy(bits, image.bgraPremultiplied.data(), image.bgraPremultiplied.size());
    out = bitmap;
    return ProviderOutcome::Success;
}

} // namespace preview3d::provider
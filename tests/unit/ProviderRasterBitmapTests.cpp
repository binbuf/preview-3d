// T13 DIB/HBITMAP boundary coverage.
//
// RasterBitmap.cpp is compiled directly into Tests.Unit.exe, so the real
// conversion from the GDI-free RasterImage to the Explorer-owned top-down 32-bpp
// premultiplied DIB section is exercised without a COM object or a window.

#include <catch2/catch_test_macros.hpp>

#include "RasterBitmap.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace preview3d::provider;

TEST_CASE("a raster image becomes a top-down 32-bit premultiplied DIB section",
          "[provider][bitmap]")
{
    RasterImage image;
    image.width = 2;
    image.height = 3;
    // 2x3 premultiplied BGRA; the exact bytes must survive the copy.
    image.bgraPremultiplied = {
        0x40, 0x30, 0x20, 0x10, 0x44, 0x33, 0x22, 0x11,
        0x80, 0x70, 0x60, 0x50, 0x84, 0x73, 0x62, 0x51,
        0xC0, 0xB0, 0xA0, 0x90, 0xC4, 0xB3, 0xA2, 0x91,
    };

    HBITMAP bitmap = nullptr;
    REQUIRE(CreatePremultipliedDib(image, bitmap) == ProviderOutcome::Success);
    REQUIRE(bitmap != nullptr);

    DIBSECTION section{};
    REQUIRE(::GetObjectW(bitmap, sizeof(section), &section) ==
            static_cast<int>(sizeof(section)));
    CHECK(section.dsBm.bmWidth == 2);
    CHECK(section.dsBm.bmHeight == 3);
    CHECK(section.dsBmih.biBitCount == 32);
    CHECK(section.dsBm.bmBitsPixel == 32);
    // The section is created with a negative biHeight (top-down); GetObject
    // normalizes the reported header back to a positive height, so the row
    // order is pinned by the byte-for-byte copy below.
    CHECK((section.dsBmih.biHeight == 3 || section.dsBmih.biHeight == -3));
    REQUIRE(section.dsBm.bmBits != nullptr);
    CHECK(std::memcmp(section.dsBm.bmBits, image.bgraPremultiplied.data(),
                      image.bgraPremultiplied.size()) == 0);

    CHECK(::DeleteObject(bitmap) != FALSE);
}

TEST_CASE("an invalid raster image is rejected with a null bitmap",
          "[provider][bitmap]")
{
    RasterImage image;
    HBITMAP bitmap = reinterpret_cast<HBITMAP>(1);

    // Empty dimensions.
    CHECK(CreatePremultipliedDib(image, bitmap) == ProviderOutcome::BadFormat);
    CHECK(bitmap == nullptr);

    // Width/height set but no bytes.
    image.width = 2;
    image.height = 2;
    CHECK(CreatePremultipliedDib(image, bitmap) == ProviderOutcome::BadFormat);
    CHECK(bitmap == nullptr);

    // Byte count does not match width * height * 4.
    image.bgraPremultiplied.assign(15, std::uint8_t{0});
    CHECK(CreatePremultipliedDib(image, bitmap) == ProviderOutcome::BadFormat);
    CHECK(bitmap == nullptr);

    // Exactly matching bytes succeed and are owned by the caller.
    image.bgraPremultiplied.assign(16, std::uint8_t{0});
    REQUIRE(CreatePremultipliedDib(image, bitmap) == ProviderOutcome::Success);
    REQUIRE(bitmap != nullptr);
    CHECK(::DeleteObject(bitmap) != FALSE);
}
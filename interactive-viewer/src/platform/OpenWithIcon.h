#pragma once

#include <cstdint>
#include <vector>

namespace open_with
{
// A shell application icon decoded off the UI thread. `bgra` is tightly packed,
// top-down, 32 bits per pixel, premultiplied alpha, matching the Direct2D
// premultiplied B8G8R8A8 format.
struct IconPixels
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;

    bool Empty() const noexcept
    {
        return width <= 0 || height <= 0 ||
            bgra.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    }
};
}

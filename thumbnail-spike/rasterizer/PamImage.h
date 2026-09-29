#pragma once

// Tiny lossless RGBA image container used by the SPIKE-8a driver and tests.
// PAM (P7) is a plain netpbm format: trivially writable, viewable in common
// tools, and free of any compression or third-party dependency. The rasterizer
// emits premultiplied BGRA; SaveBgraAsPam stores it as premultiplied RGBA so the
// alpha channel survives and the file is directly viewable.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace thumbnail_rasterizer
{

inline bool SaveBgraAsPam(const char* path, int width, int height,
                          const std::vector<std::uint8_t>& bgraPremultiplied)
{
    if (width <= 0 || height <= 0 ||
        bgraPremultiplied.size() != static_cast<std::size_t>(width) * height * 4)
        return false;
    std::FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || !file) return false;
    std::fprintf(file, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
                 width, height);
    bool ok = true;
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        const std::uint8_t b = bgraPremultiplied[i * 4 + 0];
        const std::uint8_t g = bgraPremultiplied[i * 4 + 1];
        const std::uint8_t r = bgraPremultiplied[i * 4 + 2];
        const std::uint8_t a = bgraPremultiplied[i * 4 + 3];
        const std::uint8_t rgba[4] = { r, g, b, a };
        if (std::fwrite(rgba, 1, 4, file) != 4) {
            ok = false;
            break;
        }
    }
    std::fclose(file);
    return ok;
}

inline bool LoadPamRgba(const char* path, int& width, int& height,
                        std::vector<std::uint8_t>& rgbaOut)
{
    std::FILE* file = nullptr;
    if (fopen_s(&file, path, "rb") != 0 || !file) return false;
    std::string header;
    char line[256];
    width = 0;
    height = 0;
    int depth = 0;
    while (std::fgets(line, sizeof(line), file)) {
        header += line;
        if (std::strncmp(line, "WIDTH ", 6) == 0) width = std::atoi(line + 6);
        else if (std::strncmp(line, "HEIGHT ", 7) == 0) height = std::atoi(line + 7);
        else if (std::strncmp(line, "DEPTH ", 6) == 0) depth = std::atoi(line + 6);
        else if (std::strncmp(line, "ENDHDR", 6) == 0) break;
    }
    if (width <= 0 || height <= 0 || depth != 4) {
        std::fclose(file);
        return false;
    }
    rgbaOut.resize(static_cast<std::size_t>(width) * height * 4);
    const std::size_t read = std::fread(rgbaOut.data(), 1, rgbaOut.size(), file);
    std::fclose(file);
    return read == rgbaOut.size();
}

} // namespace thumbnail_rasterizer
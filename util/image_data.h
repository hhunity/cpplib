#pragma once
#include <cstdint>
#include <vector>

enum class PixelFormat {
    rgba, // 4 bytes per pixel (R,G,B,A)
    gray, // 1 byte per pixel (luminance)
};

// Image buffer — row-major, top-left origin, 8-bit per channel.
// channels() reflects the actual layout of pixels[].
struct image_data
{
    int         width  = 0;
    int         height = 0;
    PixelFormat format = PixelFormat::rgba;
    std::vector<uint8_t> pixels; // w * h * channels() bytes

    bool empty() const { return pixels.empty(); }
    int  channels() const { return format == PixelFormat::gray ? 1 : 4; }
};

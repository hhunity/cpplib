#pragma once
#include <cstdint>
#include <vector>

// RGBA image buffer (8-bit per channel, row-major, top-left origin).
struct image_data
{
    int width  = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // w * h * 4 bytes (R,G,B,A)

    bool empty() const { return pixels.empty(); }
};

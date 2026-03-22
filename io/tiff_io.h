#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "util/image_data.h"

namespace tiff_io {

// Options for write().  All fields have sensible defaults.
struct WriteOptions
{
    uint32_t tile_size         = 512; // tile width and height in pixels (power-of-2 recommended)
    int      compression_level = 6;   // zlib level: 1 = fastest, 9 = best ratio, 0 = none
};

// Options for read().  All fields have sensible defaults.
struct ReadOptions
{
    int max_threads = 0; // 0 = hardware concurrency (no cap); positive = explicit limit
};

// Read a TIFF file into image_data (converted to RGBA internally).
// progress: optional atomic updated 0.0->1.0 as rows are decoded.
// Returns true on success.
bool read(const std::string& path, image_data& out,
          std::atomic<float>* progress = nullptr,
          const ReadOptions& opts = {});

// Write image_data (RGBA) to a TIFF file.
// Uses tiled DEFLATE with parallel per-tile compression for maximum throughput.
// Returns true on success.
bool write(const std::string& path, const image_data& img,
           const WriteOptions& opts = {});

} // namespace tiff_io

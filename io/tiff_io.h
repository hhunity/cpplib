#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "util/image_data.h"

#if defined(_WIN32)
#  ifdef IO_BUILD_DLL
#    define IO_API __declspec(dllexport)
#  else
#    define IO_API __declspec(dllimport)
#  endif
#else
#  define IO_API
#endif

namespace tiff_io {

// Options for write().  All fields have sensible defaults.
struct WriteOptions
{
    uint32_t    tile_size         = 512;              // tile width/height in pixels (must be a multiple of 16); 0 = strip layout
    int         compression_level = 6;                // zlib level: 1 = fastest, 9 = best ratio, 0 = none
    PixelFormat output_format     = PixelFormat::rgba; // output pixel format
    int         max_threads       = 0;                // 0 = hardware concurrency (no cap); positive = explicit limit
    uint32_t    rows_per_strip    = 64;               // rows per strip when tile_size == 0
};

// Options for read().  All fields have sensible defaults.
struct ReadOptions
{
    int         max_threads   = 0;                    // 0 = hardware concurrency (no cap); positive = explicit limit
    PixelFormat output_format = PixelFormat::rgba;    // desired pixel format of the returned image_data
};

// Read a TIFF file into image_data (converted to RGBA internally).
// progress: optional atomic updated 0.0->1.0 as rows are decoded.
// Returns true on success.
IO_API bool read(const std::string& path, image_data& out,
                 std::atomic<float>* progress = nullptr,
                 const ReadOptions& opts = {});

// Write image_data to a TIFF file.
// tile_size > 0: tiled DEFLATE layout with parallel per-tile compression.
// tile_size == 0: strip DEFLATE layout with parallel per-strip compression.
// Returns true on success.
IO_API bool write(const std::string& path, const image_data& img,
                  const WriteOptions& opts = {});

// Write image_data to a TIFF file without any strip/tile segmentation.
// The entire image is written as a single strip (RowsPerStrip == image height).
//   compression_level == 0: COMPRESSION_NONE (no compression at all)
//   compression_level 1-9:  COMPRESSION_ADOBE_DEFLATE, full-image single block
// Intended for performance benchmarking.  Returns true on success.
IO_API bool write_flat(const std::string& path, const image_data& img,
                       int compression_level = 0,
                       PixelFormat output_format = PixelFormat::rgba);

} // namespace tiff_io

#include "io/tiff_io.h"
#include <tiffio.h>
#include <zlib.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace tiff_io {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Number of worker threads.
// max_threads == 0: use full hardware concurrency (no artificial cap).
// max_threads  > 0: use at most that many threads.
static int worker_count(uint32_t units, int max_threads = 0) {
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    const int cap = (max_threads > 0) ? std::min(hw, max_threads) : hw;
    return std::max(1, std::min(cap, static_cast<int>(units)));
}

// Run worker(thread_id) on nthreads threads (or inline when nthreads == 1).
template<typename Fn>
static void parallel_for(int nthreads, Fn worker) {
    if (nthreads == 1) {
        worker(0);
        return;
    }
    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    for (int t = 0; t < nthreads; ++t)
        threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();
}

// Write a decoded gray/RGB/RGBA strip row into the RGBA output buffer.
static void expand_strip_rows(const uint8_t* strip_buf, uint8_t* out_pixels,
                               uint32_t w, uint32_t row0, uint32_t row1,
                               uint16_t photometric, uint16_t spp) {
    const bool is_gray = (photometric == PHOTOMETRIC_MINISBLACK ||
                          photometric == PHOTOMETRIC_MINISWHITE);
    for (uint32_t r = row0; r < row1; ++r) {
        uint8_t* dst = out_pixels + r * w * 4;
        if (is_gray) {
            const uint8_t* src = strip_buf + (r - row0) * w;
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t v = src[x];
                dst[x*4+0] = v; dst[x*4+1] = v;
                dst[x*4+2] = v; dst[x*4+3] = 255;
            }
        } else if (spp == 3) {
            const uint8_t* src = strip_buf + (r - row0) * w * 3;
            for (uint32_t x = 0; x < w; ++x) {
                dst[x*4+0] = src[x*3+0];
                dst[x*4+1] = src[x*3+1];
                dst[x*4+2] = src[x*3+2];
                dst[x*4+3] = 255;
            }
        } else { // spp == 4
            const uint8_t* src = strip_buf + (r - row0) * w * 4;
            std::memcpy(dst, src, w * 4);
        }
    }
}

// ---------------------------------------------------------------------------
// Fast path: 8-bit stripped (gray / RGB / RGBA), parallel
// ---------------------------------------------------------------------------

static bool read_strips_8bit(const std::string& path, TIFF* tif,
                              uint32_t w, uint32_t h,
                              uint16_t photometric, uint16_t spp,
                              image_data& out,
                              std::atomic<float>* progress,
                              int max_threads) {
    uint32_t rows_per_strip = h;
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rows_per_strip);
    if (rows_per_strip == 0) rows_per_strip = h;

    const uint32_t  num_strips  = TIFFNumberOfStrips(tif);
    const tmsize_t  strip_bytes = TIFFStripSize(tif);
    const int       nthreads    = worker_count(num_strips, max_threads);

    std::atomic<uint32_t> strips_done{0};
    std::atomic<bool>     had_error{false};

    parallel_for(nthreads, [&](int tid) {
        TIFF* ltif = TIFFOpen(path.c_str(), "r");
        if (!ltif) { had_error.store(true); return; }

        std::vector<uint8_t> buf(strip_bytes);

        for (uint32_t s = static_cast<uint32_t>(tid); s < num_strips; s += nthreads) {
            if (had_error.load()) break;

            if (TIFFReadEncodedStrip(ltif, s, buf.data(), strip_bytes) < 0) {
                had_error.store(true); break;
            }

            const uint32_t row0 = s * rows_per_strip;
            const uint32_t row1 = std::min(row0 + rows_per_strip, h);
            expand_strip_rows(buf.data(), out.pixels.data(), w, row0, row1, photometric, spp);

            const uint32_t done = ++strips_done;
            if (progress) progress->store(static_cast<float>(done) / num_strips);
        }

        TIFFClose(ltif);
    });

    return !had_error.load();
}

// ---------------------------------------------------------------------------
// Fast path: 8-bit tiled (gray / RGB / RGBA), parallel
// ---------------------------------------------------------------------------

static void expand_tile_rows(const uint8_t* tile_buf, uint8_t* out_pixels,
                              uint32_t img_w, uint32_t tx, uint32_t ty,
                              uint32_t tile_w, uint32_t copy_w, uint32_t copy_h,
                              uint16_t photometric, uint16_t spp) {
    const bool is_gray = (photometric == PHOTOMETRIC_MINISBLACK ||
                          photometric == PHOTOMETRIC_MINISWHITE);
    for (uint32_t r = 0; r < copy_h; ++r) {
        uint8_t* dst = out_pixels + (ty + r) * img_w * 4 + tx * 4;
        if (is_gray) {
            const uint8_t* src = tile_buf + r * tile_w;
            for (uint32_t x = 0; x < copy_w; ++x) {
                const uint8_t v = src[x];
                dst[x*4+0] = v; dst[x*4+1] = v;
                dst[x*4+2] = v; dst[x*4+3] = 255;
            }
        } else if (spp == 3) {
            const uint8_t* src = tile_buf + r * tile_w * 3;
            for (uint32_t x = 0; x < copy_w; ++x) {
                dst[x*4+0] = src[x*3+0];
                dst[x*4+1] = src[x*3+1];
                dst[x*4+2] = src[x*3+2];
                dst[x*4+3] = 255;
            }
        } else { // spp == 4
            const uint8_t* src = tile_buf + r * tile_w * 4;
            std::memcpy(dst, src, copy_w * 4);
        }
    }
}

static bool read_tiles_8bit(const std::string& path, TIFF* tif,
                             uint32_t w, uint32_t h,
                             uint16_t photometric, uint16_t spp,
                             image_data& out,
                             std::atomic<float>* progress,
                             int max_threads) {
    uint32_t tile_w = 0, tile_h = 0;
    TIFFGetField(tif, TIFFTAG_TILEWIDTH,  &tile_w);
    TIFFGetField(tif, TIFFTAG_TILELENGTH, &tile_h);
    if (tile_w == 0 || tile_h == 0) return false;

    const uint32_t ntiles   = TIFFNumberOfTiles(tif);
    const uint32_t tiles_x  = (w + tile_w - 1) / tile_w;
    const tmsize_t tile_bytes = TIFFTileSize(tif);
    const int      nthreads  = worker_count(ntiles, max_threads);

    std::atomic<uint32_t> tiles_done{0};
    std::atomic<bool>     had_error{false};

    parallel_for(nthreads, [&](int tid) {
        TIFF* ltif = TIFFOpen(path.c_str(), "r");
        if (!ltif) { had_error.store(true); return; }

        std::vector<uint8_t> buf(tile_bytes);

        for (uint32_t t = static_cast<uint32_t>(tid); t < ntiles; t += nthreads) {
            if (had_error.load()) break;

            const uint32_t tx = (t % tiles_x) * tile_w;
            const uint32_t ty = (t / tiles_x) * tile_h;

            if (TIFFReadEncodedTile(ltif, t, buf.data(), tile_bytes) < 0) {
                had_error.store(true); break;
            }

            const uint32_t copy_w = std::min(tile_w, w - tx);
            const uint32_t copy_h = std::min(tile_h, h - ty);
            expand_tile_rows(buf.data(), out.pixels.data(), w, tx, ty,
                             tile_w, copy_w, copy_h, photometric, spp);

            const uint32_t done = ++tiles_done;
            if (progress) progress->store(static_cast<float>(done) / ntiles);
        }

        TIFFClose(ltif);
    });

    return !had_error.load();
}

// ---------------------------------------------------------------------------
// Generic path: any format via TIFFReadRGBAStrip / TIFFReadRGBATile, parallel
// ---------------------------------------------------------------------------

static bool read_strips_generic(const std::string& path, TIFF* tif,
                                 uint32_t w, uint32_t h,
                                 image_data& out,
                                 std::atomic<float>* progress,
                                 int max_threads) {
    uint32_t rows_per_strip = h;
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rows_per_strip);
    if (rows_per_strip == 0) rows_per_strip = h;

    const uint32_t num_strips = TIFFNumberOfStrips(tif);
    const int      nthreads   = worker_count(num_strips, max_threads);

    std::atomic<uint32_t> strips_done{0};
    std::atomic<bool>     had_error{false};

    parallel_for(nthreads, [&](int tid) {
        TIFF* ltif = TIFFOpen(path.c_str(), "r");
        if (!ltif) { had_error.store(true); return; }

        std::vector<uint32_t> rgba(static_cast<size_t>(w) * rows_per_strip);

        for (uint32_t s = static_cast<uint32_t>(tid); s < num_strips; s += nthreads) {
            if (had_error.load()) break;

            const uint32_t row0 = s * rows_per_strip;
            if (!TIFFReadRGBAStrip(ltif, row0, rgba.data())) {
                had_error.store(true); break;
            }

            const uint32_t row1 = std::min(row0 + rows_per_strip, h);
            const uint32_t rows = row1 - row0;

            // TIFFReadRGBAStrip stores rows bottom-up within the strip; flip to top-down.
            for (uint32_t r = 0; r < rows; ++r) {
                const uint32_t* src = rgba.data() + (rows - 1 - r) * w;
                uint8_t* dst = out.pixels.data() + (row0 + r) * w * 4;
                for (uint32_t x = 0; x < w; ++x) {
                    dst[x*4+0] = TIFFGetR(src[x]);
                    dst[x*4+1] = TIFFGetG(src[x]);
                    dst[x*4+2] = TIFFGetB(src[x]);
                    dst[x*4+3] = TIFFGetA(src[x]);
                }
            }

            const uint32_t done = ++strips_done;
            if (progress) progress->store(static_cast<float>(done) / num_strips);
        }

        TIFFClose(ltif);
    });

    return !had_error.load();
}

static bool read_tiles_generic(const std::string& path, TIFF* tif,
                                uint32_t w, uint32_t h,
                                image_data& out,
                                std::atomic<float>* progress,
                                int max_threads) {
    uint32_t tile_w = 0, tile_h = 0;
    TIFFGetField(tif, TIFFTAG_TILEWIDTH,  &tile_w);
    TIFFGetField(tif, TIFFTAG_TILELENGTH, &tile_h);
    if (tile_w == 0 || tile_h == 0) return false;

    const uint32_t ntiles  = TIFFNumberOfTiles(tif);
    const uint32_t tiles_x = (w + tile_w - 1) / tile_w;
    const int      nthreads = worker_count(ntiles, max_threads);

    std::atomic<uint32_t> tiles_done{0};
    std::atomic<bool>     had_error{false};

    parallel_for(nthreads, [&](int tid) {
        TIFF* ltif = TIFFOpen(path.c_str(), "r");
        if (!ltif) { had_error.store(true); return; }

        std::vector<uint32_t> rgba(static_cast<size_t>(tile_w) * tile_h);

        for (uint32_t t = static_cast<uint32_t>(tid); t < ntiles; t += nthreads) {
            if (had_error.load()) break;

            const uint32_t tx = (t % tiles_x) * tile_w;
            const uint32_t ty = (t / tiles_x) * tile_h;

            if (!TIFFReadRGBATile(ltif, tx, ty, rgba.data())) {
                had_error.store(true); break;
            }

            const uint32_t copy_w = std::min(tile_w, w - tx);
            const uint32_t copy_h = std::min(tile_h, h - ty);

            // TIFFReadRGBATile stores rows bottom-up within the tile; flip to top-down.
            for (uint32_t r = 0; r < copy_h; ++r) {
                const uint32_t* src = rgba.data() + (tile_h - 1 - r) * tile_w;
                uint8_t* dst = out.pixels.data() + (ty + r) * w * 4 + tx * 4;
                for (uint32_t x = 0; x < copy_w; ++x) {
                    dst[x*4+0] = TIFFGetR(src[x]);
                    dst[x*4+1] = TIFFGetG(src[x]);
                    dst[x*4+2] = TIFFGetB(src[x]);
                    dst[x*4+3] = TIFFGetA(src[x]);
                }
            }

            const uint32_t done = ++tiles_done;
            if (progress) progress->store(static_cast<float>(done) / ntiles);
        }

        TIFFClose(ltif);
    });

    return !had_error.load();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool read(const std::string& path, image_data& out, std::atomic<float>* progress,
          const ReadOptions& opts) {
    TIFF* tif = TIFFOpen(path.c_str(), "r");
    if (!tif) {
        fprintf(stderr, "tiff_io::read: cannot open '%s'\n", path.c_str());
        return false;
    }

    uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH,  &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    if (w == 0 || h == 0) { TIFFClose(tif); return false; }

    uint16_t photometric      = PHOTOMETRIC_RGB;
    uint16_t bits_per_sample  = 8;
    uint16_t samples_per_pixel = 1;
    uint16_t planar_config    = PLANARCONFIG_CONTIG;
    TIFFGetField(tif, TIFFTAG_PHOTOMETRIC,     &photometric);
    TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE,   &bits_per_sample);
    TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel);
    TIFFGetField(tif, TIFFTAG_PLANARCONFIG,    &planar_config);

    out.width  = static_cast<int>(w);
    out.height = static_cast<int>(h);
    out.pixels.resize(static_cast<size_t>(w) * h * 4);

    const bool is_tiled  = TIFFIsTiled(tif) != 0;
    const bool is_8bit   = (bits_per_sample == 8);
    const bool is_contig = (planar_config == PLANARCONFIG_CONTIG);
    const bool is_gray   = (photometric == PHOTOMETRIC_MINISBLACK ||
                             photometric == PHOTOMETRIC_MINISWHITE);
    // Fast path: 8-bit contiguous gray(1ch) / RGB(3ch) / RGBA(4ch)
    const bool fast_path = is_8bit && is_contig &&
                           ((is_gray && samples_per_pixel == 1) ||
                            (photometric == PHOTOMETRIC_RGB &&
                             (samples_per_pixel == 3 || samples_per_pixel == 4)));

    const int mt = opts.max_threads;
    bool ok = false;
    if (fast_path) {
        ok = is_tiled
            ? read_tiles_8bit(path, tif, w, h, photometric, samples_per_pixel, out, progress, mt)
            : read_strips_8bit(path, tif, w, h, photometric, samples_per_pixel, out, progress, mt);
    }

    // Generic fallback: any format, any bit depth (1/2/4/16-bit, palette, CMYK, …)
    if (!ok) {
        ok = is_tiled
            ? read_tiles_generic(path, tif, w, h, out, progress, mt)
            : read_strips_generic(path, tif, w, h, out, progress, mt);
    }

    if (!ok) {
        fprintf(stderr, "tiff_io::read: failed to decode '%s'\n", path.c_str());
        TIFFClose(tif);
        return false;
    }

    TIFFClose(tif);

    // Convert to the requested output format.
    if (opts.output_format == PixelFormat::gray) {
        const size_t npixels = static_cast<size_t>(w) * h;
        std::vector<uint8_t> gray_pixels(npixels);
        for (size_t i = 0; i < npixels; ++i) {
            gray_pixels[i] = static_cast<uint8_t>(
                out.pixels[i*4+0] * 0.299f +
                out.pixels[i*4+1] * 0.587f +
                out.pixels[i*4+2] * 0.114f + 0.5f);
        }
        out.pixels = std::move(gray_pixels);
        out.format = PixelFormat::gray;
    } else {
        out.format = PixelFormat::rgba;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Pixel conversion helper used by both tile and strip write paths.
// Converts a rectangular region of img into a contiguous raw buffer.
// For tiled layout, row_stride is ts (tile width); for strip layout, row_stride is w.
// ---------------------------------------------------------------------------
static void convert_pixels(const image_data& img,
                            uint8_t* raw,
                            uint32_t src_x, uint32_t src_y,
                            uint32_t copy_w, uint32_t copy_h,
                            uint32_t w, uint32_t row_stride,
                            bool is_gray, uint32_t spp) {
    if (is_gray) {
        if (img.format == PixelFormat::gray) {
            for (uint32_t r = 0; r < copy_h; ++r) {
                const uint8_t* src = img.pixels.data() + (src_y + r) * w + src_x;
                std::memcpy(raw + r * row_stride, src, copy_w);
            }
        } else {
            for (uint32_t r = 0; r < copy_h; ++r) {
                const uint8_t* src = img.pixels.data() + (src_y + r) * w * 4 + src_x * 4;
                uint8_t*       dst = raw + r * row_stride;
                for (uint32_t x = 0; x < copy_w; ++x) {
                    dst[x] = static_cast<uint8_t>(
                        src[x*4+0] * 0.299f +
                        src[x*4+1] * 0.587f +
                        src[x*4+2] * 0.114f + 0.5f);
                }
            }
        }
    } else {
        if (img.format == PixelFormat::gray) {
            for (uint32_t r = 0; r < copy_h; ++r) {
                const uint8_t* src = img.pixels.data() + (src_y + r) * w + src_x;
                uint8_t*       dst = raw + r * row_stride * spp;
                for (uint32_t x = 0; x < copy_w; ++x) {
                    const uint8_t v = src[x];
                    dst[x*4+0] = v; dst[x*4+1] = v;
                    dst[x*4+2] = v; dst[x*4+3] = 255;
                }
            }
        } else {
            for (uint32_t r = 0; r < copy_h; ++r) {
                const uint8_t* src = img.pixels.data() + (src_y + r) * w * 4 + src_x * 4;
                std::memcpy(raw + r * row_stride * spp, src, copy_w * spp);
            }
        }
    }
}

bool write(const std::string& path, const image_data& img, const WriteOptions& opts) {
    if (img.empty()) return false;

    const uint32_t ts = opts.tile_size;
    if (ts > 0 && (ts < 16 || ts % 16 != 0)) {
        fprintf(stderr, "tiff_io::write: tile_size must be 0 (strip) or a multiple of 16 (got %u)\n", ts);
        return false;
    }

    const uint32_t w       = static_cast<uint32_t>(img.width);
    const uint32_t h       = static_cast<uint32_t>(img.height);
    const bool     is_gray = (opts.output_format == PixelFormat::gray);
    const uint32_t spp     = is_gray ? 1u : 4u;

    TIFF* tif = TIFFOpen(path.c_str(), "w");
    if (!tif) {
        fprintf(stderr, "tiff_io::write: cannot open '%s' for writing\n", path.c_str());
        return false;
    }

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH,      w);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH,     h);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, static_cast<uint16_t>(spp));
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE,   static_cast<uint16_t>(8));
    TIFFSetField(tif, TIFFTAG_ORIENTATION,     ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,     is_gray ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_COMPRESSION,     COMPRESSION_ADOBE_DEFLATE);

    if (!is_gray) {
        uint16_t extra_type = EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, static_cast<uint16_t>(1), &extra_type);
    }

    // Compressed segment buffer used by both paths.
    struct SegBuf {
        std::vector<uint8_t> data;
        uLongf               size = 0;
        bool                 ok   = false;
    };

    std::atomic<bool> had_error{false};

    if (ts > 0) {
        // ----------------------------------------------------------------
        // Tiled layout
        // ----------------------------------------------------------------
        TIFFSetField(tif, TIFFTAG_TILEWIDTH,  ts);
        TIFFSetField(tif, TIFFTAG_TILELENGTH, ts);

        const uint32_t tiles_x  = (w + ts - 1) / ts;
        const uint32_t tiles_y  = (h + ts - 1) / ts;
        const uint32_t ntiles   = tiles_x * tiles_y;
        const int      nthreads = worker_count(ntiles, opts.max_threads);

        std::vector<SegBuf> segs(ntiles);

        parallel_for(nthreads, [&](int tid) {
            std::vector<uint8_t> raw(static_cast<size_t>(ts) * ts * spp, 0);
            for (uint32_t t = static_cast<uint32_t>(tid); t < ntiles; t += nthreads) {
                if (had_error.load()) break;

                const uint32_t tx     = (t % tiles_x) * ts;
                const uint32_t ty     = (t / tiles_x) * ts;
                const uint32_t copy_w = std::min(ts, w - tx);
                const uint32_t copy_h = std::min(ts, h - ty);

                std::fill(raw.begin(), raw.end(), 0);
                convert_pixels(img, raw.data(), tx, ty, copy_w, copy_h, w, ts, is_gray, spp);

                const uLong raw_len = static_cast<uLong>(raw.size());
                segs[t].data.resize(compressBound(raw_len));
                segs[t].size = static_cast<uLongf>(segs[t].data.size());
                segs[t].ok = (compress2(segs[t].data.data(), &segs[t].size,
                                        raw.data(), raw_len,
                                        opts.compression_level) == Z_OK);
                if (!segs[t].ok) had_error.store(true);
            }
        });

        if (had_error.load()) {
            fprintf(stderr, "tiff_io::write: compression failed\n");
            TIFFClose(tif);
            return false;
        }

        for (uint32_t t = 0; t < ntiles; ++t) {
            if (TIFFWriteRawTile(tif, t,
                                 segs[t].data.data(),
                                 static_cast<tmsize_t>(segs[t].size)) < 0) {
                fprintf(stderr, "tiff_io::write: failed to write tile %u\n", t);
                TIFFClose(tif);
                return false;
            }
        }
    } else {
        // ----------------------------------------------------------------
        // Strip layout (tile_size == 0)
        // ----------------------------------------------------------------
        const uint32_t rps      = (opts.rows_per_strip > 0) ? opts.rows_per_strip : 64u;
        const uint32_t nstrips  = (h + rps - 1) / rps;
        const int      nthreads = worker_count(nstrips, opts.max_threads);

        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rps);

        std::vector<SegBuf> segs(nstrips);

        parallel_for(nthreads, [&](int tid) {
            std::vector<uint8_t> raw(static_cast<size_t>(w) * rps * spp);
            for (uint32_t s = static_cast<uint32_t>(tid); s < nstrips; s += nthreads) {
                if (had_error.load()) break;

                const uint32_t row0   = s * rps;
                const uint32_t copy_h = std::min(rps, h - row0);

                std::fill(raw.begin(), raw.end(), 0);
                convert_pixels(img, raw.data(), 0, row0, w, copy_h, w, w, is_gray, spp);

                const uLong raw_len = static_cast<uLong>(static_cast<size_t>(w) * copy_h * spp);
                segs[s].data.resize(compressBound(raw_len));
                segs[s].size = static_cast<uLongf>(segs[s].data.size());
                segs[s].ok = (compress2(segs[s].data.data(), &segs[s].size,
                                        raw.data(), raw_len,
                                        opts.compression_level) == Z_OK);
                if (!segs[s].ok) had_error.store(true);
            }
        });

        if (had_error.load()) {
            fprintf(stderr, "tiff_io::write: compression failed\n");
            TIFFClose(tif);
            return false;
        }

        for (uint32_t s = 0; s < nstrips; ++s) {
            if (TIFFWriteRawStrip(tif, s,
                                  segs[s].data.data(),
                                  static_cast<tmsize_t>(segs[s].size)) < 0) {
                fprintf(stderr, "tiff_io::write: failed to write strip %u\n", s);
                TIFFClose(tif);
                return false;
            }
        }
    }

    TIFFClose(tif);
    return true;
}

bool write_flat(const std::string& path, const image_data& img,
                int compression_level, PixelFormat output_format) {
    if (img.empty()) return false;

    const uint32_t w         = static_cast<uint32_t>(img.width);
    const uint32_t h         = static_cast<uint32_t>(img.height);
    const bool     is_gray   = (output_format == PixelFormat::gray);
    const uint32_t spp       = is_gray ? 1u : 4u;
    const bool     use_deflate = (compression_level >= 1 && compression_level <= 9);

    TIFF* tif = TIFFOpen(path.c_str(), "w");
    if (!tif) {
        fprintf(stderr, "tiff_io::write_flat: cannot open '%s' for writing\n", path.c_str());
        return false;
    }

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH,      w);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH,     h);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, static_cast<uint16_t>(spp));
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE,   static_cast<uint16_t>(8));
    TIFFSetField(tif, TIFFTAG_ORIENTATION,     ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,     is_gray ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_COMPRESSION,     use_deflate ? COMPRESSION_ADOBE_DEFLATE
                                                           : COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP,    h);  // entire image = one strip

    if (!is_gray) {
        uint16_t extra_type = EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, static_cast<uint16_t>(1), &extra_type);
    }

    // Convert all pixels into one contiguous raw buffer.
    const size_t total_bytes = static_cast<size_t>(w) * h * spp;
    std::vector<uint8_t> raw(total_bytes);
    convert_pixels(img, raw.data(), 0, 0, w, h, w, w, is_gray, spp);

    if (use_deflate) {
        // Compress the entire image as one block.
        const uLong raw_len = static_cast<uLong>(total_bytes);
        std::vector<uint8_t> compressed(compressBound(raw_len));
        uLongf comp_size = static_cast<uLongf>(compressed.size());
        if (compress2(compressed.data(), &comp_size,
                      raw.data(), raw_len, compression_level) != Z_OK) {
            fprintf(stderr, "tiff_io::write_flat: compression failed\n");
            TIFFClose(tif);
            return false;
        }
        if (TIFFWriteRawStrip(tif, 0, compressed.data(),
                              static_cast<tmsize_t>(comp_size)) < 0) {
            fprintf(stderr, "tiff_io::write_flat: failed to write compressed data\n");
            TIFFClose(tif);
            return false;
        }
    } else {
        if (TIFFWriteRawStrip(tif, 0, raw.data(),
                              static_cast<tmsize_t>(total_bytes)) < 0) {
            fprintf(stderr, "tiff_io::write_flat: failed to write image data\n");
            TIFFClose(tif);
            return false;
        }
    }

    TIFFClose(tif);
    return true;
}

} // namespace tiff_io

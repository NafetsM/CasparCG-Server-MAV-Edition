/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
* Copyright (c) 2013 Technical University of Lodz Multimedia Centre <office@cm.p.lodz.pl>
*
* Ported to CasparCG 2.5
*/

#pragma once

#include "file_operations.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace caspar { namespace replay {

// Abstract JPEG encoder interface. One instance per consumer (not thread-safe).
class jpeg_encoder {
public:
    virtual ~jpeg_encoder() = default;

    // Encode BGRX rows to JPEG. src_stride is bytes between source rows:
    //   progressive:        width * 4
    //   interlaced field:   width * 4 * 2  (every other row of the full frame)
    // Returns false on error; fills out_jpeg with compressed bytes on success.
    virtual bool encode(
        const uint8_t*        bgrx_src,
        int                   width,
        int                   height,
        int                   src_stride,
        std::vector<uint8_t>& out_jpeg) = 0;
};

// CPU encoder (libjpeg-turbo). Always available.
std::unique_ptr<jpeg_encoder> create_cpu_encoder(int quality, chroma_subsampling subsampling);

#ifdef ENABLE_NVJPEG
// GPU encoder (NVIDIA nvJPEG). Returns nullptr when no CUDA device is available.
std::unique_ptr<jpeg_encoder> create_nvjpeg_encoder(int quality, chroma_subsampling subsampling);
#endif

}} // namespace caspar::replay

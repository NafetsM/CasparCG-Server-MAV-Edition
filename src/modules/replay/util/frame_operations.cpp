/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
* Copyright (c) 2013 Technical University of Lodz Multimedia Centre <office@cm.p.lodz.pl>
*
* Ported to CasparCG 2.5
*/

#include "frame_operations.h"

#include <cstring>
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

#pragma warning(disable:4309 4244)

namespace caspar { namespace replay {

void interlace_fields(const mmx_uint8_t* src1, const mmx_uint8_t* src2,
                      mmx_uint8_t* dst,
                      uint32_t width, uint32_t height, uint32_t stride)
{
    const uint32_t full_row = width * stride;
    tbb::parallel_for(
        tbb::blocked_range<uint32_t>(0, height / 2),
        [=](const tbb::blocked_range<uint32_t>& r)
        {
            for (auto i = r.begin(); i != r.end(); ++i)
            {
                std::memcpy(dst + i * 2 * full_row,       src1 + i * full_row, full_row);
                std::memcpy(dst + (i * 2 + 1) * full_row, src2 + i * full_row, full_row);
            }
        });
}

void interlace_frames(const mmx_uint8_t* src1, const mmx_uint8_t* src2,
                      mmx_uint8_t* dst,
                      uint32_t width, uint32_t height, uint32_t stride)
{
    const uint32_t full_row = width * stride;
    tbb::parallel_for(
        tbb::blocked_range<uint32_t>(0, height / 2),
        [=](const tbb::blocked_range<uint32_t>& r)
        {
            for (auto i = r.begin(); i != r.end(); ++i)
            {
                std::memcpy(dst + i * 2 * full_row,       src1 + i * 2 * full_row,       full_row);
                std::memcpy(dst + (i * 2 + 1) * full_row, src2 + (i * 2 + 1) * full_row, full_row);
            }
        });
}

void line_double(const mmx_uint8_t* src, mmx_uint8_t* dst,
                 uint32_t width, uint32_t height, uint32_t stride)
{
    const uint32_t full_row = width * stride;
    tbb::parallel_for(
        tbb::blocked_range<uint32_t>(0, height / 2),
        [=](const tbb::blocked_range<uint32_t>& r)
        {
            for (auto i = r.begin(); i != r.end(); ++i)
            {
                std::memcpy(dst + (i * 2)     * full_row, src + i * full_row, full_row);
                std::memcpy(dst + (i * 2 + 1) * full_row, src + i * full_row, full_row);
            }
        });
}

void field_double(const mmx_uint8_t* src, mmx_uint8_t* dst,
                  uint32_t width, uint32_t height, uint32_t stride)
{
    const uint32_t full_row = width * stride;
    // height/2 - 1: the last interpolated line uses src[i+1], so we stop one early
    tbb::parallel_for(
        tbb::blocked_range<uint32_t>(0, height / 2 - 1),
        [=](const tbb::blocked_range<uint32_t>& r)
        {
            for (auto i = r.begin(); i != r.end(); ++i)
            {
                for (uint32_t j = 0; j < full_row; ++j)
                {
                    dst[i * 2 * full_row + j]       = src[i * full_row + j];
                    dst[(i * 2 + 1) * full_row + j] =
                        static_cast<uint8_t>((src[i * full_row + j] >> 1) +
                                             (src[(i + 1) * full_row + j] >> 1));
                }
            }
        });
}

// Blend: dst = src1*(level/64) + src2*((64-level)/64)
// level range: 0–64  (64 = 100% src1)
void blend_images(const mmx_uint8_t* src1, mmx_uint8_t* src2, mmx_uint8_t* dst,
                  uint32_t width, uint32_t height, uint32_t stride, uint8_t level)
{
    const uint32_t full_size = width * height * stride;
    const uint16_t lev       = static_cast<uint16_t>(level);

    tbb::parallel_for(
        tbb::blocked_range<uint32_t>(0, full_size),
        [=](const tbb::blocked_range<uint32_t>& r)
        {
            for (auto i = r.begin(); i != r.end(); ++i)
            {
                dst[i] = static_cast<uint8_t>(
                    (static_cast<int>(src1[i]) * lev        >> 6) +
                    (static_cast<int>(src2[i]) * (64 - lev) >> 6));
            }
        });
}

void black_frame(mmx_uint8_t* dst, uint32_t width, uint32_t height, uint32_t stride)
{
    std::memset(dst, 0, static_cast<size_t>(width) * height * stride);
}

#pragma warning(default:4309 4244)

}} // namespace caspar::replay

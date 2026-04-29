/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
* Copyright (c) 2013 Technical University of Lodz Multimedia Centre <office@cm.p.lodz.pl>
*
* Ported to CasparCG 2.5
*/

#pragma once

#include <cstdint>

using mmx_uint8_t = uint8_t;

namespace caspar { namespace replay {

// Interlace two separate fields into one frame (alternating lines)
void interlace_fields(const mmx_uint8_t* src1, const mmx_uint8_t* src2,
                      mmx_uint8_t* dst,
                      uint32_t width, uint32_t height, uint32_t stride);

// Interlace two full frames into one interlaced frame
void interlace_frames(const mmx_uint8_t* src1, const mmx_uint8_t* src2,
                      mmx_uint8_t* dst,
                      uint32_t width, uint32_t height, uint32_t stride);

// Double a single field to a full-height frame (interpolated odd rows)
void field_double(const mmx_uint8_t* src, mmx_uint8_t* dst,
                  uint32_t width, uint32_t height, uint32_t stride);

// Line-double a single field: each source line is copied to two consecutive
// dest lines. Used when the consumer is the channel mixer of an interlaced
// output — it extracts every-other row, so both copies must contain the field.
void line_double(const mmx_uint8_t* src, mmx_uint8_t* dst,
                 uint32_t width, uint32_t height, uint32_t stride);

// Alpha-blend two images: dst = src1*(level/64) + src2*((64-level)/64)
void blend_images(const mmx_uint8_t* src1, mmx_uint8_t* src2, mmx_uint8_t* dst,
                  uint32_t width, uint32_t height, uint32_t stride, uint8_t level);

// Fill buffer with black (zero)
void black_frame(mmx_uint8_t* dst, uint32_t width, uint32_t height, uint32_t stride);

}} // namespace caspar::replay

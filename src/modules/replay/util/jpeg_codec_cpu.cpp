/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
* Copyright (c) 2013 Technical University of Lodz Multimedia Centre <office@cm.p.lodz.pl>
*
* Ported to CasparCG 2.5
*/

#include "jpeg_codec.h"

#include <jpeglib.h>
#include <jerror.h>

#include <cstdlib>

namespace caspar { namespace replay {

class cpu_encoder_impl : public jpeg_encoder {
    int                quality_;
    chroma_subsampling subsampling_;

public:
    cpu_encoder_impl(int quality, chroma_subsampling subsampling)
        : quality_(quality)
        , subsampling_(subsampling)
    {}

    bool encode(const uint8_t* bgrx_src, int width, int height, int src_stride,
                std::vector<uint8_t>& out_jpeg) override
    {
        jpeg_compress_struct cinfo{};
        jpeg_error_mgr       jerr{};

        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_compress(&cinfo);

        // Let libjpeg-turbo allocate the output buffer; we copy into out_jpeg after.
        unsigned char* buf = nullptr;
        unsigned long  len = 0;
        jpeg_mem_dest(&cinfo, &buf, &len);

        cinfo.image_width      = static_cast<JDIMENSION>(width);
        cinfo.image_height     = static_cast<JDIMENSION>(height);
        cinfo.input_components = 4;
        cinfo.in_color_space   = JCS_EXT_BGRX;

        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, quality_, TRUE);

        auto set_samp = [&](int yh, int yv) {
            cinfo.comp_info[0].h_samp_factor = yh;
            cinfo.comp_info[0].v_samp_factor = yv;
            cinfo.comp_info[1].h_samp_factor = 1;
            cinfo.comp_info[1].v_samp_factor = 1;
            cinfo.comp_info[2].h_samp_factor = 1;
            cinfo.comp_info[2].v_samp_factor = 1;
        };
        switch (subsampling_) {
            case chroma_subsampling::Y444: set_samp(1, 1); break;
            case chroma_subsampling::Y422: set_samp(2, 1); break;
            case chroma_subsampling::Y420: set_samp(2, 2); break;
            case chroma_subsampling::Y411: set_samp(4, 1); break;
        }

        jpeg_start_compress(&cinfo, TRUE);

        JSAMPROW row[1];
        while (cinfo.next_scanline < cinfo.image_height) {
            // src_stride handles both progressive (width*4) and interlaced fields (width*8)
            row[0] = reinterpret_cast<JSAMPROW>(
                const_cast<uint8_t*>(bgrx_src) + cinfo.next_scanline * src_stride);
            jpeg_write_scanlines(&cinfo, row, 1);
        }

        jpeg_finish_compress(&cinfo);
        jpeg_destroy_compress(&cinfo);

        out_jpeg.assign(buf, buf + len);
        free(buf);
        return true;
    }
};

std::unique_ptr<jpeg_encoder> create_cpu_encoder(int quality, chroma_subsampling subsampling)
{
    return std::make_unique<cpu_encoder_impl>(quality, subsampling);
}

}} // namespace caspar::replay

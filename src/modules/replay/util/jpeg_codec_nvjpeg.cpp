/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
* Copyright (c) 2013 Technical University of Lodz Multimedia Centre <office@cm.p.lodz.pl>
*
* Ported to CasparCG 2.5
*/

#ifdef ENABLE_NVJPEG

#include "jpeg_codec.h"

#include <nvjpeg.h>
#include <cuda_runtime.h>

#include <common/log.h>

namespace caspar { namespace replay {

static nvjpegChromaSubsampling_t to_nvjpeg_css(chroma_subsampling cs)
{
    switch (cs) {
        case chroma_subsampling::Y444: return NVJPEG_CSS_444;
        case chroma_subsampling::Y422: return NVJPEG_CSS_422;
        case chroma_subsampling::Y420: return NVJPEG_CSS_420;
        case chroma_subsampling::Y411: return NVJPEG_CSS_411;
        default:                       return NVJPEG_CSS_422;
    }
}

class nvjpeg_encoder_impl : public jpeg_encoder {
    nvjpegHandle_t        handle_     = nullptr;
    nvjpegEncoderState_t  state_      = nullptr;
    nvjpegEncoderParams_t params_     = nullptr;
    cudaStream_t          stream_     = nullptr;
    uint8_t*              d_bgr_      = nullptr;  // contiguous BGR device buffer
    size_t                d_bgr_size_ = 0;
    std::vector<uint8_t>  bgr_staging_;           // host-side BGRX→BGR conversion buffer
    bool                  valid_      = false;

public:
    nvjpeg_encoder_impl(int quality, chroma_subsampling subsampling)
    {
        if (nvjpegCreate(NVJPEG_BACKEND_DEFAULT, nullptr, &handle_) != NVJPEG_STATUS_SUCCESS) {
            CASPAR_LOG(warning) << L"nvJPEG: nvjpegCreate failed — no CUDA device?";
            return;
        }
        cudaStreamCreate(&stream_);
        nvjpegEncoderStateCreate(handle_, &state_, stream_);
        nvjpegEncoderParamsCreate(handle_, &params_, stream_);
        nvjpegEncoderParamsSetQuality(params_, quality, stream_);
        nvjpegEncoderParamsSetSamplingFactors(params_, to_nvjpeg_css(subsampling), stream_);
        valid_ = true;

        cudaDeviceProp props{};
        cudaGetDeviceProperties(&props, 0);
        CASPAR_LOG(info) << L"nvJPEG encoder initialized on GPU: "
                         << props.name
                         << L" (CUDA compute " << props.major << L"." << props.minor << L")"
                         << L", quality=" << quality;
    }

    ~nvjpeg_encoder_impl()
    {
        if (!valid_) return;
        if (d_bgr_)  cudaFree(d_bgr_);
        if (params_) nvjpegEncoderParamsDestroy(params_);
        if (state_)  nvjpegEncoderStateDestroy(state_);
        if (stream_) cudaStreamDestroy(stream_);
        if (handle_) nvjpegDestroy(handle_);
    }

    bool is_valid() const { return valid_; }

    bool encode(const uint8_t* bgrx_src, int width, int height, int src_stride,
                std::vector<uint8_t>& out_jpeg) override
    {
        if (!valid_) return false;

        // Convert BGRX → BGR on CPU. nvJPEG BGRI expects tightly packed 3-byte pixels;
        // the BGRX stride trick (pitch=width*4 with BGRI) would mis-align pixel reads.
        // A CUDA kernel could replace this copy once correctness is validated.
        size_t bgr_size = static_cast<size_t>(width) * height * 3;
        bgr_staging_.resize(bgr_size);

        for (int y = 0; y < height; ++y) {
            const uint8_t* src_row = bgrx_src + static_cast<size_t>(y) * src_stride;
            uint8_t*       dst_row = bgr_staging_.data() + static_cast<size_t>(y) * width * 3;
            for (int x = 0; x < width; ++x) {
                dst_row[x * 3 + 0] = src_row[x * 4 + 0];  // B
                dst_row[x * 3 + 1] = src_row[x * 4 + 1];  // G
                dst_row[x * 3 + 2] = src_row[x * 4 + 2];  // R
            }
        }

        // Upload BGR to GPU
        if (d_bgr_size_ < bgr_size) {
            if (d_bgr_) cudaFree(d_bgr_);
            cudaMalloc(&d_bgr_, bgr_size);
            d_bgr_size_ = bgr_size;
        }
        cudaMemcpyAsync(d_bgr_, bgr_staging_.data(), bgr_size,
                        cudaMemcpyHostToDevice, stream_);

        // Encode interleaved BGR
        nvjpegImage_t nv_img{};
        nv_img.channel[0] = d_bgr_;
        nv_img.pitch[0]   = static_cast<unsigned int>(width * 3);

        nvjpegEncodeImage(handle_, state_, params_, &nv_img,
                          NVJPEG_INPUT_BGRI, width, height, stream_);

        // Retrieve bitstream length, then data
        size_t length = 0;
        nvjpegEncodeRetrieveBitstream(handle_, state_, nullptr, &length, stream_);
        cudaStreamSynchronize(stream_);

        if (length == 0) return false;

        out_jpeg.resize(length);
        nvjpegEncodeRetrieveBitstream(handle_, state_, out_jpeg.data(), &length, stream_);
        cudaStreamSynchronize(stream_);

        return length > 0;
    }
};

std::unique_ptr<jpeg_encoder> create_nvjpeg_encoder(int quality, chroma_subsampling subsampling)
{
    auto enc = std::make_unique<nvjpeg_encoder_impl>(quality, subsampling);
    if (!enc->is_valid())
        return nullptr;
    return enc;
}

}} // namespace caspar::replay

#endif // ENABLE_NVJPEG

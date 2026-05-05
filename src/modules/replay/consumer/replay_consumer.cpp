/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
* Copyright (c) 2013 Technical University of Lodz Multimedia Centre <office@cm.p.lodz.pl>
*
* Ported to CasparCG 2.5
*/

#include "replay_consumer.h"

#include "../util/frame_operations.h"
#include "../util/file_operations.h"
#include "../util/jpeg_codec.h"

#include <core/frame/frame.h>
#include <core/consumer/frame_consumer.h>
#include <core/consumer/channel_info.h>
#include <core/video_format.h>
#include <core/monitor/monitor.h>

#include <common/executor.h>
#include <common/env.h>
#include <common/future.h>
#include <common/diagnostics/graph.h>
#include <common/param.h>
#include <common/log.h>

#include <boost/algorithm/string.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/lexical_cast.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

namespace caspar { namespace replay {

#define REPLAY_FRAME_BUFFER     32
#define REPLAY_JPEG_QUALITY     90
#define REPLAY_JPEG_SUBSAMPLING chroma_subsampling::Y422

struct replay_consumer : public core::frame_consumer
{
    core::monitor::state                        state_;
    mutable std::mutex                          state_mutex_;
    core::video_format_desc                     format_desc_;
    std::wstring                                filename_;
    std::atomic<uint64_t>                       framenum_{ 0 };
    short                                       quality_;
    chroma_subsampling                          subsampling_;
    mjpeg_file_handle                           output_file_      = nullptr;
    mjpeg_file_handle                           output_idx_file_  = nullptr;
    bool                                        file_open_        = false;
    uint8_t                                     field_mode_       = 3; // 1=LFF, 2=UFF, 3=progressive
    bool                                        dropped_field_a_  = false;
    executor                                    encode_executor_;
    spl::shared_ptr<diagnostics::graph>         graph_;
    boost::posix_time::ptime                    start_timecode_;
    int64_t                                     start_hardware_ts_ = -1; // µs from hardware clock at first frame
    std::unique_ptr<jpeg_encoder>               encoder_;
    std::vector<uint8_t>                        jpeg_buf_;

public:
    replay_consumer(const std::wstring& filename, short quality, chroma_subsampling subsampling)
        : filename_(filename)
        , quality_(quality)
        , subsampling_(subsampling)
        , encode_executor_(L"replay_consumer")
    {
        encode_executor_.set_capacity(REPLAY_FRAME_BUFFER);

#ifdef ENABLE_NVJPEG
        encoder_ = create_nvjpeg_encoder(quality_, subsampling_);
        if (!encoder_) {
            CASPAR_LOG(warning) << L"replay_consumer: nvJPEG unavailable, falling back to CPU encoder (libjpeg-turbo)";
            encoder_ = create_cpu_encoder(quality_, subsampling_);
        } else {
            CASPAR_LOG(info) << L"replay_consumer: JPEG encoder = nvJPEG (GPU)";
        }
#else
        encoder_ = create_cpu_encoder(quality_, subsampling_);
        CASPAR_LOG(info) << L"replay_consumer: JPEG encoder = libjpeg-turbo (CPU)";
#endif

        graph_->set_color("frame-time",     diagnostics::color(0.1f, 1.0f, 0.1f));
        graph_->set_color("dropped-frame",  diagnostics::color(0.3f, 0.6f, 0.3f));
        graph_->set_color("buffered-video", diagnostics::color(0.1f, 0.1f, 0.8f));
        graph_->set_text(print());
        diagnostics::register_graph(graph_);
    }

    ~replay_consumer() override
    {
        encode_executor_.stop();

        if (output_file_)
            safe_fclose(output_file_);
        if (output_idx_file_)
            safe_fclose(output_idx_file_);

        CASPAR_LOG(info) << print() << L" Successfully Uninitialized.";
    }

    // ── frame_consumer interface ──────────────────────────────────────────────

    // CasparCG 2.5 signature
    void initialize(const core::video_format_desc& format_desc,
                    const core::channel_info& /*channel_info*/,
                    int /*port_index*/) override
    {
        format_desc_ = format_desc;

        auto mav_path = env::media_folder() + filename_ + L".mav";
        auto idx_path = env::media_folder() + filename_ + L".idx";

        output_file_ = safe_fopen(mav_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ);
        if (!output_file_)
        {
            CASPAR_LOG(error) << print() << L" Cannot open " << mav_path << L" for writing";
            return;
        }

        output_idx_file_ = safe_fopen(idx_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ);
        if (!output_idx_file_)
        {
            CASPAR_LOG(error) << print() << L" Cannot open " << idx_path << L" for writing";
            safe_fclose(output_file_);
            output_file_ = nullptr;
            return;
        }

        file_open_      = true;
        start_timecode_ = boost::posix_time::microsec_clock::universal_time();

        // Field-order detection: HD interlaced (>=720 lines) is upper-field-first,
        // SD interlaced (PAL/NTSC) is lower-field-first.
        if (format_desc_.field_count == 2)
            field_mode_ = (format_desc_.height >= 720) ? 2 : 1;
        else
            field_mode_ = 3; // progressive

        write_index_header(output_idx_file_, &format_desc_, start_timecode_,
                           format_desc_.audio_channels,
                           format_desc_.audio_sample_rate,
                           field_mode_);

        CASPAR_LOG(info) << print() << L" Recording "
                         << format_desc_.width << L"x" << format_desc_.height
                         << (field_mode_ == 3 ? L" progressive"
                             : field_mode_ == 2 ? L" interlaced (UFF)"
                                                : L" interlaced (LFF)")
                         << L" @ " << format_desc_.fps << L" fps, "
                         << format_desc_.audio_channels << L"ch @ "
                         << format_desc_.audio_sample_rate << L" Hz";
    }

    std::future<bool> send(const core::video_field field, core::const_frame frame) override
    {
        if (!file_open_)
            return make_ready_future(true);

        // Pick JPEG encoding mode + output height based on field type.
        //   progressive: full frame
        //   interlaced UFF (field_mode=2): field::a → upper rows, field::b → lower rows
        //   interlaced LFF (field_mode=1): field::a → lower rows, field::b → upper rows
        mjpeg_process_mode mode;
        uint32_t           out_height;
        if (field == core::video_field::progressive)
        {
            mode       = mjpeg_process_mode::PROGRESSIVE;
            out_height = format_desc_.height;
        }
        else
        {
            const bool field_a       = (field == core::video_field::a);
            const bool upper_first   = (field_mode_ == 2);
            const bool encode_upper  = (field_a == upper_first);
            mode       = encode_upper ? mjpeg_process_mode::UPPER : mjpeg_process_mode::LOWER;
            out_height = format_desc_.height / 2;
        }

        // Keep field pairs aligned: if field::a was dropped, also drop field::b
        // to avoid writing field::b as if it were the first field of the next frame.
        if (field == core::video_field::b && dropped_field_a_)
        {
            dropped_field_a_ = false;
            graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
            return make_ready_future(true);
        }

        if (encode_executor_.size() < encode_executor_.capacity())
        {
            if (field == core::video_field::a)
                dropped_field_a_ = false;

            encode_executor_.begin_invoke([this, frame, mode, out_height]
            {
                auto t0 = std::chrono::high_resolution_clock::now();

                encode_video_frame(frame, mode, out_height);

                auto elapsed = std::chrono::duration<double>(
                    std::chrono::high_resolution_clock::now() - t0).count();

                graph_->set_text(print());
                graph_->set_value("frame-time", elapsed * 0.5 * format_desc_.fps);

                std::lock_guard<std::mutex> lock(state_mutex_);
                state_["profiler/time"] = { elapsed, 1.0 / format_desc_.fps };
                state_["file/time"]     = static_cast<double>(framenum_) / format_desc_.fps;
                state_["file/frame"]    = static_cast<int32_t>(framenum_.load());
                state_["file/fps"]      = format_desc_.fps;
                state_["file/path"]     = filename_;
            });

            graph_->set_value("buffered-video",
                static_cast<double>(encode_executor_.size()) /
                static_cast<double>(encode_executor_.capacity()));
        }
        else
        {
            if (field == core::video_field::a)
                dropped_field_a_ = true;
            graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
        }

        return make_ready_future(true);
    }

    bool has_synchronization_clock() const override { return false; }

    std::wstring print() const override
    {
        return L"replay_consumer[" + filename_ + L".mav|" +
               std::to_wstring(framenum_.load()) + L"]";
    }

    std::wstring name() const override { return L"replay"; }

    int index() const override { return 150; }

    core::monitor::state state() const override
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return state_;
    }

private:
    void encode_video_frame(core::const_frame   frame,
                            mjpeg_process_mode  mode,
                            uint32_t            out_height)
    {
        // Determine the source row pointer and stride for this field:
        //   progressive  → start at row 0, stride = width * 4
        //   interlaced UPPER-field → start at row 0, stride = width * 4 * 2
        //   interlaced LOWER-field → start at row 1, stride = width * 4 * 2
        const int     pixel_stride = format_desc_.width * 4;
        const uint8_t* bgrx_src    = frame.image_data(0).begin();
        int            src_stride  = pixel_stride;

        if (mode == mjpeg_process_mode::LOWER)
            bgrx_src += pixel_stride;   // advance one full row to reach the lower field
        if (mode != mjpeg_process_mode::PROGRESSIVE)
            src_stride = pixel_stride * 2;

        if (!encoder_->encode(bgrx_src, static_cast<int>(format_desc_.width),
                              static_cast<int>(out_height), src_stride, jpeg_buf_)) {
            CASPAR_LOG(error) << print() << L" JPEG encode failed";
            return;
        }

        long long pos = write_frame_encoded(
            output_file_,
            jpeg_buf_.data(),
            jpeg_buf_.size(),
            frame.audio_data().begin(),
            static_cast<uint32_t>(frame.audio_data().size() * sizeof(int32_t)));

        // Derive per-frame timestamp from hardware clock
        int64_t hw_ts  = frame.hardware_timestamp();
        int64_t ts_us;
        if (hw_ts >= 0) {
            if (start_hardware_ts_ < 0)
                start_hardware_ts_ = hw_ts;
            ts_us = hw_ts - start_hardware_ts_;
        } else {
            ts_us = INT64_MIN; // gap — no hardware timestamp available
        }

        write_index_v4(output_idx_file_, { pos, ts_us });
        ++framenum_;
    }
};

// ── Factory ───────────────────────────────────────────────────────────────────

spl::shared_ptr<core::frame_consumer> create_consumer(
    const std::vector<std::wstring>&                          params,
    const core::video_format_repository&                      /*format_repository*/,
    const std::vector<spl::shared_ptr<core::video_channel>>  /*channels*/,
    const core::channel_info&                                 /*channel_info*/)
{
    if (params.size() < 2 || !boost::iequals(params[0], L"REPLAY"))
        return core::frame_consumer::empty();

    std::wstring        filename    = params[1];
    short               quality     = REPLAY_JPEG_QUALITY;
    chroma_subsampling  subsampling = REPLAY_JPEG_SUBSAMPLING;

    if (params.size() > 2)
    {
        for (std::size_t i = 2; i < params.size(); ++i)
        {
            if (boost::iequals(params[i], L"SUBSAMPLING") && i + 1 < params.size())
            {
                ++i;
                if      (params[i] == L"444") subsampling = chroma_subsampling::Y444;
                else if (params[i] == L"422") subsampling = chroma_subsampling::Y422;
                else if (params[i] == L"420") subsampling = chroma_subsampling::Y420;
                else if (params[i] == L"411") subsampling = chroma_subsampling::Y411;
            }
            else if (boost::iequals(params[i], L"QUALITY") && i + 1 < params.size())
            {
                quality = boost::lexical_cast<short>(params[++i]);
            }
        }
    }

    return spl::make_shared<replay_consumer>(filename, quality, subsampling);
}

}} // namespace caspar::replay

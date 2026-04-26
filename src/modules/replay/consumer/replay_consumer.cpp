/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
* Copyright (c) 2013 Technical University of Lodz Multimedia Centre <office@cm.p.lodz.pl>
*
* Ported to CasparCG 2.5
*/

#include "replay_consumer.h"

#include "../util/frame_operations.h"
#include "../util/file_operations.h"

#include <core/frame/frame.h>
#include <core/consumer/frame_consumer.h>
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
    executor                                    encode_executor_;
    spl::shared_ptr<diagnostics::graph>         graph_;
    boost::posix_time::ptime                    start_timecode_;

public:
    replay_consumer(const std::wstring& filename, short quality, chroma_subsampling subsampling)
        : filename_(filename)
        , quality_(quality)
        , subsampling_(subsampling)
        , encode_executor_(L"replay_consumer")
    {
        encode_executor_.set_capacity(REPLAY_FRAME_BUFFER);

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
                    const core::audio_channel_layout& /*channel_layout*/,
                    int /*channel_index*/) override
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

        write_index_header(output_idx_file_, &format_desc_, start_timecode_,
                           format_desc_.audio_channels);
    }

    std::future<bool> send(core::const_frame frame) override
    {
        if (!file_open_)
            return make_ready_future(true);

        if (encode_executor_.size() < encode_executor_.capacity())
        {
            encode_executor_.begin_invoke([=]
            {
                auto t0 = std::chrono::high_resolution_clock::now();

                encode_video_frame(frame);

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
    void encode_video_frame(core::const_frame frame)
    {
        long long written = write_frame(
            output_file_,
            format_desc_.width,
            format_desc_.height,
            frame.image_data(0).begin(),
            quality_,
            mjpeg_process_mode::PROGRESSIVE,
            subsampling_,
            frame.audio_data().begin(),
            static_cast<uint32_t>(frame.audio_data().size() * sizeof(int32_t)));

        write_index(output_idx_file_, written);
        ++framenum_;
    }
};

// ── Factory ───────────────────────────────────────────────────────────────────

spl::shared_ptr<core::frame_consumer> create_consumer(
    const std::vector<std::wstring>&                  params,
    std::vector<spl::shared_ptr<core::video_channel>> channels,
    const core::video_format_repository&              /*format_repository*/)
{
    if (params.empty() || !boost::iequals(params[0], L"REPLAY"))
        return core::frame_consumer::empty();

    std::wstring        filename    = L"REPLAY";
    short               quality     = REPLAY_JPEG_QUALITY;
    chroma_subsampling  subsampling = REPLAY_JPEG_SUBSAMPLING;

    if (params.size() > 1)
    {
        filename = params[1];

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

/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
* Copyright (c) 2013 Technical University of Lodz Multimedia Centre <office@cm.p.lodz.pl>
*
* Ported to CasparCG 2.5
*/

#include "replay_producer.h"

#include "../util/frame_operations.h"
#include "../util/file_operations.h"

#include <core/frame/draw_frame.h>
#include <core/frame/pixel_format.h>
#include <core/frame/frame_factory.h>
#include <core/frame/frame.h>
#include <core/video_format.h>
#include <core/monitor/monitor.h>

#include <common/array.h>
#include <common/future.h>
#include <common/env.h>
#include <common/array.h>
#include <common/diagnostics/graph.h>
#include <common/log.h>

#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace caspar { namespace replay {

struct replay_producer : public core::frame_producer
{
    // ── State ─────────────────────────────────────────────────────────────────
    core::monitor::state                               state_;
    mutable std::mutex                                 state_mutex_;

    const std::wstring                                 filename_;
    core::draw_frame                                   frame_      { core::draw_frame::empty() };
    core::draw_frame                                   last_frame_ { core::draw_frame::empty() };

    std::mutex                                         frame_buffer_mutex_;
    std::queue<std::pair<core::draw_frame, uint64_t>>  frame_buffer_;
    bool                                               frame_stable_   = false;

    mjpeg_file_handle                                  in_file_        = nullptr;
    mjpeg_file_handle                                  in_idx_file_    = nullptr;

    spl::shared_ptr<mjpeg_file_header>                 index_header_;
    spl::shared_ptr<mjpeg_file_header_ex>              index_header_ex_;
    spl::shared_ptr<core::frame_factory>               frame_factory_;

    std::atomic<uint64_t>  framenum_          { 0 };
    std::atomic<uint64_t>  real_framenum_     { 0 };
    std::atomic<uint64_t>  real_last_framenum_{ 0 };
    std::atomic<uint64_t>  first_framenum_    { 0 };
    std::atomic<uint64_t>  last_framenum_     { 0 };
    std::atomic<uint64_t>  result_framenum_   { 0 };
    std::atomic<int>       runstate_          { 0 };

    uint8_t*  leftovers_           = nullptr;
    int       leftovers_duration_  = 0;
    int32_t*  leftovers_audio_     = nullptr;
    uint32_t  leftovers_audio_size_= 0;

    bool     interlaced_    = false;
    int      audio_         = 0;
    float    speed_         = 1.0f;
    float    abs_speed_     = 1.0f;
    int      frame_divider_ = 1;
    int      frame_multiplier_ = 1;
    bool     reverse_       = false;
    bool     seeked_        = false;

    spl::shared_ptr<diagnostics::graph> graph_;
    std::thread*                         decoder_ = nullptr;

    // ── Constructor ───────────────────────────────────────────────────────────
#pragma warning(disable:4244)
    explicit replay_producer(
        const spl::shared_ptr<core::frame_factory>& frame_factory,
        const std::wstring& filename,
        const int           sign,
        uint64_t            start_frame,
        uint64_t            last_frame_count,
        float               start_speed,
        int                 audio = 0)
        : filename_(filename)
        , frame_factory_(frame_factory)
    {
        in_file_ = safe_fopen(filename_.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE);
        if (!in_file_)
        {
            CASPAR_LOG(error) << print() << L" Video file not found: " << filename_;
            CASPAR_THROW_EXCEPTION(file_not_found());
        }

        auto idx_path = boost::filesystem::path(filename_)
                            .replace_extension(L".idx").wstring();
        in_idx_file_ = safe_fopen(idx_path.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE);
        if (!in_idx_file_)
        {
            CASPAR_LOG(error) << print() << L" Index file not found: " << idx_path;
            safe_fclose(in_file_);
            CASPAR_THROW_EXCEPTION(file_not_found());
        }

        // Wait until the index file has data (supports live recording)
        uint64_t size = 0;
        while (size == 0)
        {
            size = static_cast<uint64_t>(
                boost::filesystem::file_size(
                    boost::filesystem::path(filename_)
                        .replace_extension(L".idx")));
            if (size == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Read headers
        {
            mjpeg_file_header* hdr = nullptr;
            read_index_header(in_idx_file_, &hdr);
            index_header_ = spl::shared_ptr<mjpeg_file_header>(hdr);

            CASPAR_LOG(info) << print() << L" File starts at: "
                             << boost::posix_time::to_iso_wstring(
                                    index_header_->begin_timecode);

            mjpeg_file_header_ex* hdr_ex = nullptr;
            if (index_header_->version >= 2)
            {
                read_index_header_ex(in_idx_file_, &hdr_ex);
                index_header_ex_ = spl::shared_ptr<mjpeg_file_header_ex>(hdr_ex);
                CASPAR_LOG(info) << print() << L" Audio channels: "
                                 << index_header_ex_->audio_channels;
            }
            else
            {
                hdr_ex = new mjpeg_file_header_ex{};
                hdr_ex->audio_channels = 0;
                index_header_ex_ = spl::shared_ptr<mjpeg_file_header_ex>(hdr_ex);
            }
        }

        interlaced_ = (index_header_->field_mode != 3);
        audio_      = audio;

        set_playback_speed(start_speed);

        if (start_frame > 0)
            seek(interlaced_ ? start_frame * 2 : start_frame, sign);

        if (last_frame_count > 0)
        {
            last_framenum_ = start_frame + last_frame_count;
            if (interlaced_) last_framenum_ = last_framenum_ * 2;
        }

        graph_->set_color("frame-time", diagnostics::color(0.1f, 1.0f, 0.1f));
        graph_->set_color("underflow",  diagnostics::color(0.6f, 0.3f, 0.9f));
        graph_->set_text(print());
        diagnostics::register_graph(graph_);

        // Decoder thread
        decoder_ = new std::thread([this]
        {
            while (runstate_ == 0)
            {
                std::size_t cur_size = 0;
                {
                    std::lock_guard<std::mutex> lock(frame_buffer_mutex_);
                    cur_size = frame_buffer_.size();
                }

                if (cur_size < REPLAY_PRODUCER_BUFFER_SIZE)
                {
                    try
                    {
                        auto t0 = std::chrono::high_resolution_clock::now();
                        real_last_framenum_ = static_cast<uint64_t>(
                            length_index(in_idx_file_));
                        if (interlaced_ && !(real_last_framenum_ & 1))
                            --real_last_framenum_;

                        auto frame_pair = render_frame(0);
                        {
                            std::lock_guard<std::mutex> lock(frame_buffer_mutex_);
                            frame_buffer_.push(frame_pair);
                        }

                        double elapsed = std::chrono::duration<double>(
                            std::chrono::high_resolution_clock::now() - t0).count();
                        update_diag(elapsed * 0.5 * index_header_->fps);
                    }
                    catch (...)
                    {
                        CASPAR_LOG(error) << print()
                            << L" Unknown exception in decoding thread";
                    }
                }
                else
                {
                    int sleep_ms = static_cast<int>(500.0 / index_header_->fps);
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                }
            }
        });
    }
#pragma warning(default:4244)

    ~replay_producer() override
    {
        runstate_ = 1;
        if (decoder_ && decoder_->joinable())
            decoder_->join();
        delete decoder_;

        if (leftovers_)       { delete[] leftovers_;       leftovers_       = nullptr; }
        if (leftovers_audio_) { delete[] leftovers_audio_; leftovers_audio_ = nullptr; }

        safe_fclose(in_file_);
        safe_fclose(in_idx_file_);
    }

    // ── Frame construction ────────────────────────────────────────────────────

    core::draw_frame make_frame(uint8_t*       frame_data,
                                uint32_t       /*size*/,
                                uint32_t       width,
                                uint32_t       height,
                                const int32_t* audio_data        = nullptr,
                                uint32_t       audio_data_length = 0)
    {
        // CasparCG 2.5 native format is BGRA (4 bytes per pixel)
        core::pixel_format_desc desc(core::pixel_format::bgra);
        desc.planes.push_back(core::pixel_format_desc::plane(width, height, 4));
        core::mutable_frame mutable_frame = frame_factory_->create_frame(this, desc);

        // Convert RGB → BGRA
        auto* dst = mutable_frame.image_data(0).begin();
        for (uint32_t i = 0; i < width * height; ++i)
        {
            dst[i * 4 + 0] = frame_data[i * 3 + 2]; // B
            dst[i * 4 + 1] = frame_data[i * 3 + 1]; // G
            dst[i * 4 + 2] = frame_data[i * 3 + 0]; // R
            dst[i * 4 + 3] = 0xFF;                   // A
        }

        if (audio_ && audio_data_length > 0 && audio_data)
        {
            auto num_samples = audio_data_length / sizeof(int32_t);
            std::vector<int32_t> audio_vec(audio_data, audio_data + num_samples);
            mutable_frame.audio_data() = caspar::array<int32_t>(std::move(audio_vec));
        }

        frame_ = core::draw_frame(std::move(mutable_frame));
        return frame_;
    }

    // ── AMCP call handler ─────────────────────────────────────────────────────

    std::future<std::wstring> call(const std::vector<std::wstring>& params) override
    {
        return make_ready_future(
            std::move(do_call(boost::algorithm::join(params, L" "))));
    }

    std::wstring do_call(const std::wstring& param)
    {
        static const boost::wregex speed_exp (L"SPEED\\s+(?<VALUE>[\\d.-]+)",          boost::regex::icase);
        static const boost::wregex pause_exp (L"PAUSE",                                boost::regex::icase);
        static const boost::wregex seek_exp  (L"SEEK\\s+(?<SIGN>[\\+\\-\\|])?(?<VALUE>[\\d]+)", boost::regex::icase);
        static const boost::wregex length_exp(L"LENGTH\\s+(?<VALUE>[\\d]+)",           boost::regex::icase);
        static const boost::wregex audio_exp (L"AUDIO\\s+(?<VALUE>[\\d]+)",            boost::regex::icase);

        boost::wsmatch m;

        if (boost::regex_match(param, m, pause_exp))
        {
            set_playback_speed(0.0f);
            return L"";
        }
        if (boost::regex_match(param, m, speed_exp))
        {
            if (!m["VALUE"].str().empty())
                set_playback_speed(boost::lexical_cast<float>(m["VALUE"].str()));
            return L"";
        }
        if (boost::regex_match(param, m, seek_exp))
        {
            int sign = 0;
            if (!m["SIGN"].str().empty())
            {
                auto s = m["SIGN"].str();
                if      (s == L"+") sign =  1;
                else if (s == L"|") sign = -2;
                else if (s == L"-") sign = -1;
            }
            if (!m["VALUE"].str().empty())
            {
                uint64_t pos = boost::lexical_cast<uint64_t>(m["VALUE"].str());
                seek(interlaced_ ? pos * 2 : pos, sign);
            }
            return L"";
        }
        if (boost::regex_match(param, m, length_exp))
        {
            if (!m["VALUE"].str().empty())
            {
                long long lf = boost::lexical_cast<long long>(m["VALUE"].str());
                if (lf == 0)
                    last_framenum_ = 0;
                else
                {
                    last_framenum_ = first_framenum_ / 2 + lf;
                    if (interlaced_) last_framenum_ = last_framenum_ * 2;
                }
            }
            return L"";
        }
        if (boost::regex_match(param, m, audio_exp))
        {
            if (!m["VALUE"].str().empty())
                audio_ = (boost::lexical_cast<int>(m["VALUE"].str()) == 1) ? 1 : 0;
            return L"";
        }

        CASPAR_THROW_EXCEPTION(invalid_argument());
    }

    // ── Playback control ──────────────────────────────────────────────────────

    void seek(uint64_t frame_pos, int sign)
    {
        uint64_t rlfn = real_last_framenum_;
        if (sign == 0)
        {
            framenum_ = (frame_pos > rlfn) ? rlfn : frame_pos;
        }
        else if (sign == -2)
        {
            framenum_ = (rlfn < frame_pos + 4) ? 0 : rlfn - frame_pos - 4;
        }
        else if (sign == -1)
        {
            framenum_ = (framenum_ < frame_pos) ? 0 : framenum_ - frame_pos;
        }
        else // sign == 1
        {
            uint64_t next = framenum_ + frame_pos;
            framenum_ = (next > rlfn) ? rlfn : next;
        }

        if (seek_index(in_idx_file_, static_cast<long long>(framenum_), FILE_BEGIN))
            CASPAR_LOG(error) << L"[replay] seek_index failed at frame " << framenum_;

        first_framenum_ = framenum_.load();
        seeked_         = true;
    }

    void set_playback_speed(float speed)
    {
        speed_     = speed;
        abs_speed_ = std::fabs(speed);
        if (speed != 0.0f)
            frame_divider_ = std::abs(static_cast<int>(1.0f / speed));
        else
            frame_divider_ = 0;
        frame_multiplier_ = std::abs(static_cast<int>(speed));
        reverse_          = (speed < 0.0f);
    }

    void update_diag(double elapsed)
    {
        graph_->set_text(print());
        graph_->set_value("frame-time", elapsed * 0.5);

        uint64_t rfn   = real_framenum_;
        uint64_t rlfn  = real_last_framenum_;
        uint64_t ffn   = first_framenum_;
        uint64_t lfn   = last_framenum_;
        double   fps   = index_header_->fps;
        int      div   = interlaced_ ? 2 : 1;

        std::lock_guard<std::mutex> lock(state_mutex_);
        state_["profiler/time"] = { elapsed, 1.0 / fps };
        state_["file/time"]  = { static_cast<double>(rfn / div) / fps,
                                  static_cast<double>((lfn - ffn) / div) / fps };
        state_["file/frame"] = { static_cast<int32_t>(rfn / div),
                                  static_cast<int32_t>(rlfn / div) };
        state_["file/vframe"]= { static_cast<int32_t>((rfn - ffn) / div),
                                  static_cast<int32_t>(((lfn > 0 ? lfn : rlfn) - ffn) / div) };
        state_["file/fps"]   = fps;
        state_["file/path"]  = filename_;
        state_["file/speed"] = speed_;
    }

    // ── Frame rendering helpers ───────────────────────────────────────────────

    void move_to_next_frame()
    {
        int mult = (frame_multiplier_ > 1) ? frame_multiplier_ : 1;
        bool seek_needed = false;
        uint64_t rlfn = real_last_framenum_;

        if (reverse_)
        {
            framenum_ = (framenum_ < static_cast<uint64_t>(mult)) ? 0 : framenum_ - mult;
            seek_needed = true;
        }
        else
        {
            if (framenum_ + mult >= rlfn)
            {
                framenum_   = rlfn;
                seek_needed = true;
            }
            else
            {
                framenum_ += mult;
                if (mult > 1) seek_needed = true;
            }
        }

        if (seek_needed &&
            seek_index(in_idx_file_, static_cast<long long>(framenum_), FILE_BEGIN))
            CASPAR_LOG(error) << L"[replay] move_to_next_frame: seek_index failed";
    }

    void sync_to_frame()
    {
        if (!interlaced_ || framenum_ % 2 == 0) return;

        if (framenum_ + 1 >= real_last_framenum_)
        {
            seek_index(in_idx_file_, -1, FILE_CURRENT);
            --framenum_;
        }
        else
        {
            read_index(in_idx_file_);
            ++framenum_;
        }
    }

    void proper_interlace(const mmx_uint8_t* f1, const mmx_uint8_t* f2, mmx_uint8_t* dst)
    {
        if (index_header_->field_mode == 1) // lower field first
            interlace_fields(f2, f1, dst, index_header_->width, index_header_->height, 3);
        else
            interlace_fields(f1, f2, dst, index_header_->width, index_header_->height, 3);
    }

#pragma warning(disable:4244)
    bool slow_motion_playback(uint8_t*  result,
                              int32_t** result_audio,
                              uint32_t* result_audio_size)
    {
        uint32_t frame_size = index_header_->width * index_header_->height * 3;
        auto buffer1 = std::make_unique<uint8_t[]>(frame_size);
        auto buffer2 = std::make_unique<uint8_t[]>(frame_size);
        black_frame(buffer1.get(), index_header_->width, index_header_->height, 3);
        std::memcpy(buffer2.get(), buffer1.get(), frame_size);

        *result_audio      = nullptr;
        *result_audio_size = 0;

        int filled = 0;

        // ── Use leftover from previous call ──────────────────────────────────
        if (leftovers_)
        {
            blend_images(leftovers_, buffer1.get(), buffer2.get(),
                         index_header_->width, index_header_->height, 3, 64);
            std::swap(buffer1, buffer2);

            // Copy leftover audio as the current result audio
            if (leftovers_audio_ && leftovers_audio_size_ > 0)
            {
                *result_audio      = new int32_t[leftovers_audio_size_ / 4];
                *result_audio_size = leftovers_audio_size_;
                std::memcpy(*result_audio, leftovers_audio_, leftovers_audio_size_);
            }

            filled += leftovers_duration_;

            if (filled >= 64)
            {
                // Leftover covers the full output frame
                leftovers_duration_ = filled - 64;
                std::memcpy(result, buffer2.get(), frame_size);
                return true;
            }
            else
            {
                // Leftover used up — clear it
                delete[] leftovers_;       leftovers_       = nullptr;
                delete[] leftovers_audio_; leftovers_audio_ = nullptr;
                leftovers_duration_   = 0;
                leftovers_audio_size_ = 0;
            }
        }

        int frame_duration = static_cast<int>((1.0f / abs_speed_) * 64.0f);
        if (frame_duration <= 0) frame_duration = 1;

        // ── Decode frames until output frame is filled ────────────────────────
        while (filled < 64)
        {
            long long field_pos = read_index(in_idx_file_);
            if (field_pos == -1)
            {
                // EOF
                if (*result_audio) { delete[] *result_audio; *result_audio = nullptr; }
                *result_audio_size = 0;
                return false;
            }
            move_to_next_frame();
            seek_frame(in_file_, field_pos, FILE_BEGIN);

            uint8_t* field     = nullptr;
            uint32_t fw = 0, fh = 0, audio_sz = 0;
            int32_t* audio_buf = nullptr;
            read_frame(in_file_, &fw, &fh, &field, &audio_sz, &audio_buf);

            auto field_guard = std::unique_ptr<uint8_t[]>(field);
            auto audio_guard = std::unique_ptr<int32_t[]>(audio_buf);

            if (!field_guard)
                break;

            if (interlaced_)
            {
                auto doubled = std::make_unique<uint8_t[]>(frame_size);
                field_double(field_guard.get(), doubled.get(),
                             index_header_->width, index_header_->height, 3);
                field_guard = std::move(doubled);
            }

            // How much of this frame fits into the output?
            int remaining   = 64 - filled;
            int contribution = std::min(frame_duration, remaining);
            uint8_t level   = static_cast<uint8_t>(contribution);

            blend_images(field_guard.get(), buffer2.get(), buffer1.get(),
                         index_header_->width, index_header_->height, 3, level);
            std::swap(buffer1, buffer2);

            // Use this frame's audio as output audio (last one wins)
            if (*result_audio) delete[] *result_audio;
            if (audio_sz > 0 && audio_guard)
            {
                *result_audio      = new int32_t[audio_sz / 4];
                *result_audio_size = audio_sz;
                std::memcpy(*result_audio, audio_guard.get(), audio_sz);
            }

            filled += frame_duration;

            // If this frame extends beyond the output — save the excess as leftover
            if (filled > 64)
            {
                leftovers_duration_   = filled - 64;
                leftovers_audio_size_ = audio_sz;

                // Save pixel leftover
                delete[] leftovers_;
                leftovers_ = new uint8_t[frame_size];
                std::memcpy(leftovers_, field_guard.get(), frame_size);

                // Save audio leftover (separate copy)
                delete[] leftovers_audio_;
                if (audio_sz > 0 && audio_guard)
                {
                    leftovers_audio_ = new int32_t[audio_sz / 4];
                    std::memcpy(leftovers_audio_, audio_guard.get(), audio_sz);
                }
                else
                {
                    leftovers_audio_ = nullptr;
                }
            }
        }

        std::memcpy(result, buffer2.get(), frame_size);
        return true;
    }
#pragma warning(default:4244)

    std::pair<core::draw_frame, uint64_t> render_frame(int /*hints*/)
    {
        bool eof = !seeked_ && (
            speed_ == 0.0f ||
            (reverse_  && framenum_ == 0) ||
            (!reverse_ && framenum_ >= real_last_framenum_) ||
            (last_framenum_ > 0 &&  reverse_ && first_framenum_ >= framenum_) ||
            (last_framenum_ > 0 && !reverse_ && last_framenum_  <= framenum_));

        if (eof && frame_stable_)
            return { core::draw_frame::still(frame_), framenum_ };

        seeked_ = false;

        bool slow = (abs_speed_ > 0.0f && std::fmod(abs_speed_, 1.0f) != 0.0f);

        if (slow)
        {
            uint32_t frame_size = index_header_->width * index_header_->height * 3;
            auto field1 = std::make_unique<uint8_t[]>(frame_size);
            int32_t* audio1 = nullptr; uint32_t audio1_sz = 0;

            if (!slow_motion_playback(field1.get(), &audio1, &audio1_sz))
                return { frame_, framenum_ };

            auto audio1_guard = std::unique_ptr<int32_t[]>(audio1);

            if (!interlaced_)
            {
                make_frame(field1.get(), frame_size,
                           index_header_->width, index_header_->height,
                           audio1, audio1_sz);
                frame_stable_ = true;
                return { frame_, framenum_ };
            }

            auto field2 = std::make_unique<uint8_t[]>(frame_size);
            int32_t* audio2 = nullptr; uint32_t audio2_sz = 0;

            if (!slow_motion_playback(field2.get(), &audio2, &audio2_sz))
            {
                make_frame(field1.get(), frame_size,
                           index_header_->width, index_header_->height,
                           audio1, audio1_sz);
                frame_stable_ = true;
                return { frame_, framenum_ };
            }

            auto audio2_guard = std::unique_ptr<int32_t[]>(audio2);
            uint32_t total_audio = audio1_sz + audio2_sz;
            auto audio_combined = std::make_unique<int32_t[]>(total_audio / 4);
            std::memcpy(audio_combined.get(),                   audio1, audio1_sz);
            std::memcpy(audio_combined.get() + audio1_sz / 4,  audio2, audio2_sz);

            auto full_frame = std::make_unique<uint8_t[]>(frame_size);
            interlace_frames(field1.get(), field2.get(), full_frame.get(),
                             index_header_->width, index_header_->height, 3);
            make_frame(full_frame.get(), frame_size,
                       index_header_->width, index_header_->height,
                       audio_combined.get(), total_audio);
            frame_stable_ = false;
            return { frame_, framenum_ };
        }

        // ── Normal / fast playback ─────────────────────────────────────────────

        if (leftovers_)
        {
            delete[] leftovers_;       leftovers_       = nullptr;
            delete[] leftovers_audio_; leftovers_audio_ = nullptr;
            leftovers_audio_size_ = 0;
        }

        if (abs_speed_ >= 1.0f)
            sync_to_frame();

        long long field1_pos = read_index(in_idx_file_);
        if (field1_pos == -1)
            return { frame_, framenum_ };

        move_to_next_frame();
        seek_frame(in_file_, field1_pos, FILE_BEGIN);

        uint8_t* field1 = nullptr;
        uint32_t fw = 0, fh = 0, audio1_sz = 0;
        int32_t* audio1 = nullptr;
        uint32_t field1_size = read_frame(in_file_, &fw, &fh, &field1, &audio1_sz, &audio1);

        auto field1_guard  = std::unique_ptr<uint8_t[]>(field1);
        auto audio1_guard  = std::unique_ptr<int32_t[]>(audio1);

        if (!field1)
            return { frame_, framenum_ };

        if (!interlaced_)
        {
            make_frame(field1, field1_size, fw, fh, audio1, audio1_sz);
            frame_stable_ = true;
            return { frame_, framenum_ };
        }

        // Paused or EOF + interlaced → field double
        if ((speed_ == 0.0f || eof) && interlaced_)
        {
            auto full = std::make_unique<uint8_t[]>(field1_size * 2);
            field_double(field1, full.get(), fw, fh, 3);
            make_frame(full.get(), field1_size * 2, fw, fh);
            frame_stable_ = true;
            return { frame_, framenum_ };
        }

        long long field2_pos = read_index(in_idx_file_);
        move_to_next_frame();
        seek_frame(in_file_, field2_pos, FILE_BEGIN);

        uint8_t* field2 = nullptr;
        uint32_t audio2_sz = 0;
        int32_t* audio2 = nullptr;
        uint32_t field2_size = read_frame(in_file_, &fw, &fh, &field2, &audio2_sz, &audio2);

        auto field2_guard = std::unique_ptr<uint8_t[]>(field2);
        auto audio2_guard = std::unique_ptr<int32_t[]>(audio2);

        if (!field2)
            return { frame_, framenum_ };

        uint32_t total_audio = audio1_sz + audio2_sz;
        auto audio_combined = std::make_unique<int32_t[]>(total_audio / 4);
        std::memcpy(audio_combined.get(),                   audio1, audio1_sz);
        std::memcpy(audio_combined.get() + audio1_sz / 4,  audio2, audio2_sz);

        auto full_frame = std::make_unique<uint8_t[]>(field1_size + field2_size);
        proper_interlace(field1, field2, full_frame.get());
        make_frame(full_frame.get(), field1_size + field2_size, fw, fh,
                   audio_combined.get(), total_audio);
        frame_stable_ = false;
        return { frame_, framenum_ };
    }

    // ── frame_producer interface ──────────────────────────────────────────────

    core::draw_frame receive_impl(const core::video_field /*field*/,
                                  int                    /*nb_samples*/) override
    {
        std::lock_guard<std::mutex> lock(frame_buffer_mutex_);
        if (frame_buffer_.empty())
        {
            ++result_framenum_;
            graph_->set_tag(diagnostics::tag_severity::WARNING, "underflow");
            return last_frame_;
        }

        auto [frm, fn] = frame_buffer_.front();
        frame_buffer_.pop();
        last_frame_   = frm;
        real_framenum_ = fn;
        ++result_framenum_;
        return frm;
    }

    core::draw_frame last_frame(const core::video_field /*field*/) override
    {
        return core::draw_frame::still(last_frame_);
    }

#pragma warning(disable:4244)
    uint32_t nb_frames() const override
    {
        if (last_framenum_ > 0 && speed_ != 0.0f)
        {
            uint64_t span = last_framenum_ - first_framenum_;
            if (interlaced_) span /= 2;
            return static_cast<uint32_t>(span / speed_);
        }
        return std::numeric_limits<uint32_t>::max();
    }
#pragma warning(default:4244)

    bool is_ready() override { return true; }

    std::wstring print() const override
    {
        uint64_t fn = interlaced_ ? real_framenum_ / 2 : real_framenum_.load();
        return L"replay_producer[" + filename_ +
               L"|" + std::to_wstring(fn) +
               L"|" + std::to_wstring(speed_) + L"]";
    }

    std::wstring name() const override { return L"replay"; }

    core::monitor::state state() const override
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return state_;
    }
};

// ── Factories ─────────────────────────────────────────────────────────────────

core::draw_frame create_thumbnail(
    const core::frame_producer_dependencies& /*deps*/,
    const std::wstring&                      /*media_file*/)
{
    return core::draw_frame::empty();
}

spl::shared_ptr<core::frame_producer> create_producer(
    const core::frame_producer_dependencies& deps,
    const std::vector<std::wstring>&         params)
{
    static const std::vector<std::wstring> extensions = { L"mav" };

    std::wstring filename = env::media_folder() + params.at(0);

    auto ext_it = std::find_if(
        extensions.begin(), extensions.end(),
        [&](const std::wstring& ex)
        {
            return boost::filesystem::is_regular_file(
                boost::filesystem::path(filename).replace_extension(ex));
        });

    if (ext_it == extensions.end())
        return core::frame_producer::empty();

    int      sign        = 0;
    int      audio       = 0;
    uint64_t start_frame = 0;
    uint64_t last_frame  = 0;
    float    start_speed = 1.0f;

    for (std::size_t i = 0; i < params.size(); ++i)
    {
        if (boost::iequals(params[i], L"SEEK") && i + 1 < params.size())
        {
            static const boost::wregex seek_exp(L"(?<SIGN>[\\|])?(?<VALUE>[\\d]+)",
                                                boost::regex::icase);
            boost::wsmatch m;
            if (boost::regex_match(params[i + 1], m, seek_exp))
            {
                if (!m["SIGN"].str().empty()) sign = -2;
                if (!m["VALUE"].str().empty())
                    start_frame = boost::lexical_cast<uint64_t>(m["VALUE"].str());
            }
        }
        else if (boost::iequals(params[i], L"SPEED") && i + 1 < params.size())
        {
            start_speed = boost::lexical_cast<float>(params[i + 1]);
        }
        else if (boost::iequals(params[i], L"LENGTH") && i + 1 < params.size())
        {
            last_frame = boost::lexical_cast<uint64_t>(params[i + 1]);
        }
        else if (boost::iequals(params[i], L"AUDIO") && i + 1 < params.size())
        {
            audio = (boost::lexical_cast<int>(params[i + 1]) == 1) ? 1 : 0;
        }
    }

    return spl::make_shared<replay_producer>(
        deps.frame_factory,
        filename + L"." + *ext_it,
        sign,
        start_frame,
        last_frame,
        start_speed,
        audio);
}

}} // namespace caspar::replay

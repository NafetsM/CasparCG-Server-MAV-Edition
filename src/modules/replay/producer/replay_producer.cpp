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

#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
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
    bool     is_v4_         = false; // true when index uses 16-byte v4 entries
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
        int                 audio = -1) // -1 = auto, 0 = forced off, 1 = forced on
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
                read_index_header_ex(in_idx_file_, index_header_->version, &hdr_ex);
                index_header_ex_ = spl::shared_ptr<mjpeg_file_header_ex>(hdr_ex);
                CASPAR_LOG(info) << print() << L" Audio: "
                                 << index_header_ex_->audio_channels << L" channels @ "
                                 << index_header_ex_->audio_sample_rate << L" Hz";

                if (index_header_->version > MAV_VERSION_CURRENT)
                {
                    CASPAR_LOG(warning) << print()
                        << L" File version " << static_cast<int>(index_header_->version)
                        << L" is newer than supported " << static_cast<int>(MAV_VERSION_CURRENT)
                        << L" — index seeks may be incorrect.";
                }
            }
            else
            {
                hdr_ex = new mjpeg_file_header_ex{};
                hdr_ex->audio_channels    = 0;
                hdr_ex->audio_sample_rate = 48000;
                index_header_ex_ = spl::shared_ptr<mjpeg_file_header_ex>(hdr_ex);
            }
        }

        interlaced_ = (index_header_->field_mode != 3);
        is_v4_      = (index_header_->version >= 4);
        // audio == -1 → auto-enable when file has audio channels.
        // audio ==  0 → user explicitly disabled.
        // audio ==  1 → user explicitly enabled.
        audio_      = (audio < 0) ? (index_header_ex_->audio_channels > 0 ? 1 : 0)
                                  : audio;

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
                            length_index_impl());
                        if (interlaced_ && !(real_last_framenum_ & 1))
                            --real_last_framenum_;

                        // At the live edge of an open-ended recording, don't
                        // flood the buffer with still frames — wait for the
                        // consumer to write the next entry instead.  When the
                        // buffer runs dry, receive_impl falls back to
                        // last_frame_ automatically, which is far smoother
                        // than the stop-go jitter caused by draining a buffer
                        // full of identical stills.
                        if (!seeked_ && speed_ != 0.0f && last_framenum_ == 0 &&
                            !reverse_ && framenum_ >= real_last_framenum_)
                        {
                            // Sleep for one entry period so the consumer has
                            // time to write the next entry before we check again.
                            // fps stores the frame rate; for interlaced files the
                            // entry rate is fps*2 (one entry per field).
                            double entry_rate = interlaced_
                                ? index_header_->fps * 2.0
                                : index_header_->fps;
                            int sleep_ms = static_cast<int>(1000.0 / entry_rate);
                            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                            continue;
                        }

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
                    double entry_rate = interlaced_
                        ? index_header_->fps * 2.0
                        : index_header_->fps;
                    int sleep_ms = static_cast<int>(1000.0 / entry_rate);
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
        static const boost::wregex play_exp     (L"PLAY",                                 boost::regex::icase);
        static const boost::wregex speed_exp    (L"SPEED\\s+(?<VALUE>[\\d.-]+)",          boost::regex::icase);
        static const boost::wregex pause_exp    (L"PAUSE",                                boost::regex::icase);
        static const boost::wregex seek_exp     (L"SEEK\\s+(?<SIGN>[\\+\\-\\|])?(?<VALUE>[\\d]+)(?<UNIT>ms|s)?", boost::regex::icase);
        static const boost::wregex seek_abs_exp (L"SEEK_ABS\\s+(?<VALUE>[\\d]+)",        boost::regex::icase);
        static const boost::wregex length_exp   (L"LENGTH\\s+(?<VALUE>[\\d]+)",           boost::regex::icase);
        static const boost::wregex audio_exp    (L"AUDIO\\s+(?<VALUE>[\\d]+)",            boost::regex::icase);

        boost::wsmatch m;

        if (boost::regex_match(param, m, play_exp))
        {
            if (speed_ == 0.0f)
                set_playback_speed(1.0f);
            return L"";
        }
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
                auto unit = m["UNIT"].str();
                if (!unit.empty() && is_v4_)
                {
                    // Time-based seek: "|1500ms" or "|3s" (offset back from live edge)
                    int64_t val_raw = boost::lexical_cast<int64_t>(m["VALUE"].str());
                    int64_t offset_us = boost::iequals(unit, L"ms")
                                        ? val_raw * 1000LL
                                        : val_raw * 1000000LL;

                    uint64_t rlfn_do = real_last_framenum_.load();
                    int64_t live_ts = (rlfn_do > 0)
                        ? read_timestamp_at(in_idx_file_, static_cast<long long>(rlfn_do - 1))
                        : INT64_MIN;
                    int64_t target_us = (sign == -2 && live_ts != INT64_MIN)
                                        ? live_ts - offset_us
                                        : offset_us;
                    if (target_us < 0) target_us = 0;

                    uint64_t tf = seek_by_time(target_us);
                    framenum_ = tf;
                    seek_index_v4(in_idx_file_, static_cast<long long>(tf), FILE_BEGIN);
                    first_framenum_ = framenum_.load();
                    seeked_         = true;
                }
                else
                {
                    uint64_t pos = boost::lexical_cast<uint64_t>(m["VALUE"].str());
                    seek(interlaced_ ? pos * 2 : pos, sign);
                }
            }
            return L"";
        }
        if (boost::regex_match(param, m, seek_abs_exp))
        {
            if (!m["VALUE"].str().empty() && is_v4_)
            {
                int64_t target_epoch_ms = boost::lexical_cast<int64_t>(m["VALUE"].str());
                auto    epoch           = boost::posix_time::ptime(
                                              boost::gregorian::date(1970, 1, 1));
                int64_t begin_epoch_ms  = (index_header_->begin_timecode - epoch)
                                              .total_milliseconds();
                int64_t target_us       = (target_epoch_ms - begin_epoch_ms) * 1000LL;

                if (target_us < 0) {
                    framenum_ = 0;
                    seek_index_v4(in_idx_file_, 0LL, FILE_BEGIN);
                } else {
                    uint64_t tf = seek_by_time(target_us);
                    framenum_ = tf;
                    seek_index_v4(in_idx_file_, static_cast<long long>(tf), FILE_BEGIN);
                }
                first_framenum_ = framenum_.load();
                seeked_         = true;
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

        if (seek_index_impl(static_cast<long long>(framenum_), FILE_BEGIN))
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

    // ── Version-aware index wrappers ──────────────────────────────────────────

    long long length_index_impl() const
    {
        return is_v4_ ? length_index_v4(in_idx_file_) : length_index(in_idx_file_);
    }

    int seek_index_impl(long long frame, uint32_t origin)
    {
        return is_v4_ ? seek_index_v4(in_idx_file_, frame, origin)
                      : seek_index(in_idx_file_, frame, origin);
    }

    long long read_index_impl()
    {
        if (is_v4_) {
            auto e = read_index_v4(in_idx_file_);
            return e.file_offset;
        }
        return read_index(in_idx_file_);
    }

    // Binary search for the first frame whose timestamp >= target_us.
    uint64_t seek_by_time(int64_t target_us)
    {
        uint64_t lo = 0;
        uint64_t hi = real_last_framenum_.load();
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            int64_t  ts  = read_timestamp_at(in_idx_file_, static_cast<long long>(mid));
            if (ts == INT64_MIN || ts < target_us)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
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

        if (is_v4_) {
            int64_t live_ts = (rlfn > 0)
                ? read_timestamp_at(in_idx_file_, static_cast<long long>(rlfn - 1))
                : INT64_MIN;
            int64_t cur_ts  = read_timestamp_at(in_idx_file_, static_cast<long long>(rfn));
            if (live_ts != INT64_MIN && cur_ts != INT64_MIN) {
                auto    epoch          = boost::posix_time::ptime(boost::gregorian::date(1970, 1, 1));
                int64_t begin_epoch_ms = (index_header_->begin_timecode - epoch).total_milliseconds();
                state_["file/live_edge_absolute_ms"] = begin_epoch_ms + live_ts / 1000;
                state_["file/time_behind_live_ms"]   = (live_ts - cur_ts) / 1000;
            }
            state_["file/gap_detected"] =
                (read_timestamp_at(in_idx_file_, static_cast<long long>(rfn)) == INT64_MIN);
        }
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
            seek_index_impl(static_cast<long long>(framenum_), FILE_BEGIN))
            CASPAR_LOG(error) << L"[replay] move_to_next_frame: seek_index failed";
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
            // blend_images: max level = 63 (= 100% src1)
            blend_images(leftovers_, buffer1.get(), buffer2.get(),
                         index_header_->width, index_header_->height, 3, 63);

            if (leftovers_audio_ && leftovers_audio_size_ > 0)
            {
                *result_audio      = new int32_t[leftovers_audio_size_ / 4];
                *result_audio_size = leftovers_audio_size_;
                std::memcpy(*result_audio, leftovers_audio_, leftovers_audio_size_);
            }

            filled += leftovers_duration_;

            if (filled >= 64)
            {
                leftovers_duration_ = filled - 64;
                std::memcpy(result, buffer2.get(), frame_size);
                return true;
            }
            else
            {
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
            long long field_pos = read_index_impl();
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
                // Use line-doubling so both even- and odd-row extractions by
                // the channel mixer yield the field data unchanged.
                line_double(field_guard.get(), doubled.get(),
                            index_header_->width, index_header_->height, 3);
                field_guard = std::move(doubled);
            }

            // Level berechnung exakt wie im Original (max level = 63):
            // filled==0: erster Frame → volle Gewichtung (63)
            // sonst: wie viel vom Frame noch in den Output-Slot passt
            uint8_t level;
            if (filled == 0)
                level = 63;
            else
                level = static_cast<uint8_t>((frame_duration + filled) <= 64
                            ? frame_duration : 64 - filled);

            // blend_images(src1=neuer Frame, src2=bisheriger Blend, dst=Ergebnis)
            // Ergebnis landet in buffer2 (wie im Original)
            blend_images(field_guard.get(), buffer2.get(), buffer2.get(),
                         index_header_->width, index_header_->height, 3, level);

            // Audio: letzter Frame gewinnt
            if (*result_audio) delete[] *result_audio;
            if (audio_sz > 0 && audio_guard)
            {
                *result_audio      = new int32_t[audio_sz / 4];
                *result_audio_size = audio_sz;
                std::memcpy(*result_audio, audio_guard.get(), audio_sz);
            }

            filled += frame_duration;

            // Wenn dieser Frame über den Output-Slot hinausgeht → Leftover speichern
            if (filled > 64)
            {
                leftovers_duration_   = filled - 64;
                leftovers_audio_size_ = audio_sz;

                delete[] leftovers_;
                leftovers_ = new uint8_t[frame_size];
                std::memcpy(leftovers_, field_guard.get(), frame_size);

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

        const uint32_t full_w  = index_header_->width;
        const uint32_t full_h  = index_header_->height;
        const uint32_t full_sz = full_w * full_h * 3;

        const bool slow = (abs_speed_ > 0.0f && std::fmod(abs_speed_, 1.0f) != 0.0f);

        // ── Slow motion ───────────────────────────────────────────────────────
        // slow_motion_playback returns one rendered field worth of data
        // (already field-doubled to full size for interlaced files).
        if (slow)
        {
            auto rendered = std::make_unique<uint8_t[]>(full_sz);
            int32_t* audio = nullptr; uint32_t audio_sz = 0;

            if (!slow_motion_playback(rendered.get(), &audio, &audio_sz))
                return { frame_, framenum_ };

            auto audio_guard = std::unique_ptr<int32_t[]>(audio);
            make_frame(rendered.get(), full_sz, full_w, full_h, audio, audio_sz);
            frame_stable_ = false;
            return { frame_, framenum_ };
        }

        // ── Normal / fast playback ────────────────────────────────────────────
        // One .mav entry per call — for interlaced files the entry is one field
        // (half height); the channel mixer for a 1080i50 output extracts
        // alternate rows so a line-doubled field placed in a full-height frame
        // is correctly seen as the right field, regardless of which row set the
        // mixer pulls.

        if (leftovers_)
        {
            delete[] leftovers_;       leftovers_       = nullptr;
            delete[] leftovers_audio_; leftovers_audio_ = nullptr;
            leftovers_audio_size_ = 0;
        }

        long long field_pos = read_index_impl();
        if (field_pos == -1)
            return { frame_, framenum_ };

        move_to_next_frame();
        seek_frame(in_file_, field_pos, FILE_BEGIN);

        uint8_t* field = nullptr;
        uint32_t fw = 0, fh = 0, audio_sz = 0;
        int32_t* audio = nullptr;
        uint32_t field_size = read_frame(in_file_, &fw, &fh, &field, &audio_sz, &audio);

        auto field_guard = std::unique_ptr<uint8_t[]>(field);
        auto audio_guard = std::unique_ptr<int32_t[]>(audio);

        if (!field)
            return { frame_, framenum_ };

        if (!interlaced_)
        {
            // Progressive file: JPEG entry already covers the full frame.
            make_frame(field, field_size, fw, fh, audio, audio_sz);
            frame_stable_ = (speed_ == 0.0f || eof);
            return { frame_, framenum_ };
        }

        // Interlaced file: line-double the half-height field into a full frame.
        auto full = std::make_unique<uint8_t[]>(full_sz);
        line_double(field, full.get(), full_w, full_h, 3);
        make_frame(full.get(), full_sz, full_w, full_h, audio, audio_sz);
        frame_stable_ = (speed_ == 0.0f || eof);
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
            // framenum_ counts .mav entries; receive_impl pops one per call.
            // For interlaced files this means one entry per channel field tick.
            uint64_t span = last_framenum_ - first_framenum_;
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
    int      audio       = -1; // -1 = auto (enable when file has audio)
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

/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
* Copyright (c) 2013 Technical University of Lodz Multimedia Centre <office@cm.p.lodz.pl>
*
* Ported to CasparCG 2.5
*/

#pragma once

#include <cstdint>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <core/video_format.h>

// ── Platform-specific I/O ────────────────────────────────────────────────────
#ifdef _WIN32
#   define NOMINMAX
#   include <Windows.h>
    // Use Win32 API for large-file support on Windows
#   define REPLAY_IO_WINAPI
#endif

#ifndef REPLAY_IO_WINAPI
#   include <common/utf.h>
#   include <cstdint>
#   define _FILE_OFFSET_BITS 64
#   ifdef _WIN32
#       ifndef fopen64
#           define fopen64 fopen
#       endif
#       ifndef fseek64
#           define fseek64 _fseeki64
#       endif
#       ifndef ftell64
#           define ftell64 _ftelli64
#       endif
#   elif defined(__x86_64__)
        // On 64-bit Linux, fopen/fseek/ftell are already 64-bit via _FILE_OFFSET_BITS=64
#       ifndef fopen64
#           define fopen64 fopen
#       endif
#       ifndef fseek64
#           define fseek64 fseek
#       endif
#       ifndef ftell64
#           define ftell64 ftell
#       endif
#   endif
#   ifndef FILE_CURRENT
#       define FILE_CURRENT SEEK_CUR
#   endif
#   ifndef FILE_BEGIN
#       define FILE_BEGIN SEEK_SET
#   endif
#   ifndef GENERIC_READ
#       define GENERIC_READ  0x80000000
#   endif
#   ifndef GENERIC_WRITE
#       define GENERIC_WRITE 0x40000000
#   endif
#   ifndef FILE_SHARE_READ
#       define FILE_SHARE_READ  0x00000001
#   endif
#   ifndef FILE_SHARE_WRITE
#       define FILE_SHARE_WRITE 0x00000002
#   endif
    // DWORD is a Windows type; provide a portable equivalent for the POSIX path
    using DWORD = std::uint32_t;
#endif

#ifdef REPLAY_IO_WINAPI
    using mjpeg_file_handle = HANDLE;
#else
    using mjpeg_file_handle = FILE*;
#endif

namespace caspar { namespace replay {

// ── File format structures ────────────────────────────────────────────────────

#pragma pack(push, 1)

struct mjpeg_file_header
{
    char                        magick[4];      // 'OMAV'
    uint8_t                     version;        // 3 (current)
    uint32_t                    width;
    uint32_t                    height;
    double                      fps;            // fields per second (interlaced) or frames per second (progressive)
    uint8_t                     field_mode = 3; // 1=lower-first, 2=upper-first, 3=progressive
    boost::posix_time::ptime    begin_timecode;
};

// Extended header — version >= 2
struct mjpeg_file_header_ex
{
    char    video_fourcc[4];    // 'mjpg'
    char    audio_fourcc[4];    // 'in32'
    int     audio_channels;
    int     audio_sample_rate;  // Hz (added in v3; v2 files default to 48000)
};

static constexpr uint8_t MJPEG_FILE_VERSION = 3;

#pragma pack(pop)

// ── Enums ─────────────────────────────────────────────────────────────────────

enum class mjpeg_process_mode
{
    PROGRESSIVE,
    UPPER,
    LOWER
};

enum class chroma_subsampling
{
    Y444,
    Y422,
    Y420,
    Y411
};

// Keep old-style names as aliases so existing call-sites compile unchanged
static constexpr auto PROGRESSIVE = mjpeg_process_mode::PROGRESSIVE;
static constexpr auto UPPER       = mjpeg_process_mode::UPPER;
static constexpr auto LOWER       = mjpeg_process_mode::LOWER;
static constexpr auto Y444        = chroma_subsampling::Y444;
static constexpr auto Y422        = chroma_subsampling::Y422;
static constexpr auto Y420        = chroma_subsampling::Y420;
static constexpr auto Y411        = chroma_subsampling::Y411;

// ── I/O helpers ──────────────────────────────────────────────────────────────

mjpeg_file_handle safe_fopen(const wchar_t* filename, uint32_t mode, uint32_t share_flags);
void              safe_fclose(mjpeg_file_handle handle);

// ── Index operations ─────────────────────────────────────────────────────────

void      write_index_header(mjpeg_file_handle idx,
                             const core::video_format_desc* format_desc,
                             boost::posix_time::ptime       start_timecode,
                             int                            audio_channels,
                             int                            audio_sample_rate,
                             uint8_t                        field_mode);

void      write_index(mjpeg_file_handle idx, long long offset);

long long read_index(mjpeg_file_handle idx);
long long tell_index(mjpeg_file_handle idx);
long long length_index(mjpeg_file_handle idx);
int       seek_index(mjpeg_file_handle idx, long long frame, uint32_t origin);

int       read_index_header   (mjpeg_file_handle idx, mjpeg_file_header**    out);
int       read_index_header_ex(mjpeg_file_handle idx, uint8_t version, mjpeg_file_header_ex** out);

// ── Frame (MAV data) operations ───────────────────────────────────────────────

long long write_frame(mjpeg_file_handle outfile,
                      uint32_t          width,
                      uint32_t          height,
                      const uint8_t*    image,
                      short             quality,
                      mjpeg_process_mode mode,
                      chroma_subsampling subsampling,
                      const int32_t*    audio_data,
                      uint32_t          audio_data_length);

uint32_t  read_frame(mjpeg_file_handle infile,
                     uint32_t*  width,
                     uint32_t*  height,
                     uint8_t**  image,
                     uint32_t*  audio_size,
                     int32_t**  audio);

long long tell_frame(mjpeg_file_handle infile);
int       seek_frame(mjpeg_file_handle infile, long long offset, uint32_t origin);

}} // namespace caspar::replay

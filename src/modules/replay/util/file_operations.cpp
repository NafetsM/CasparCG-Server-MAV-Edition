/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
* Copyright (c) 2013 Technical University of Lodz Multimedia Centre <office@cm.p.lodz.pl>
*
* Ported to CasparCG 2.5
*/

#include "file_operations.h"

#include <jpeglib.h>
#include <jerror.h>
#include <setjmp.h>

#include <common/env.h>
#include <common/log.h>

#pragma warning(disable:4800)
#pragma warning(disable:4267)

namespace caspar { namespace replay {

// ── JPEG I/O manager structs ──────────────────────────────────────────────────

static constexpr int VIDEO_OUTPUT_BUF_SIZE = 4096;
static constexpr int VIDEO_INPUT_BUF_SIZE  = 4096;

struct tag_src_mgr {
    jpeg_source_mgr   pub;
    mjpeg_file_handle infile;
    JOCTET*           buffer;
    boolean           start_of_file;
};
using src_ptr = tag_src_mgr*;

struct tag_dest_mgr {
    jpeg_destination_mgr pub;
    mjpeg_file_handle    outfile;
    JOCTET*              buffer;
};
using dest_ptr = tag_dest_mgr*;

#pragma warning(disable:4324)
struct error_mgr {
    jpeg_error_mgr pub;
    jmp_buf        setjmp_buffer;
};
#pragma warning(default:4324)

// ── File open / close ─────────────────────────────────────────────────────────

#pragma warning(disable:4706)
mjpeg_file_handle safe_fopen(const wchar_t* filename, uint32_t mode, uint32_t share_flags)
{
#ifdef REPLAY_IO_WINAPI
    DWORD creation = (mode == GENERIC_WRITE) ? CREATE_ALWAYS : OPEN_EXISTING;
    mjpeg_file_handle handle = CreateFileW(filename, mode, share_flags,
                                           nullptr, creation, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        CASPAR_LOG(error) << L"[replay] Cannot open file, WinAPI error: " << err;
        return nullptr;
    }
    return handle;
#else
    const char* fmode = (mode == GENERIC_WRITE) ? "wb" : "rb";
    return fopen64(u8(filename).c_str(), fmode);
#endif
}
#pragma warning(default:4706)

void safe_fclose(mjpeg_file_handle handle)
{
#ifdef REPLAY_IO_WINAPI
    if (handle && handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
#else
    if (handle)
        fclose(handle);
#endif
}

// ── Index header ─────────────────────────────────────────────────────────────

void write_index_header(mjpeg_file_handle idx,
                        const core::video_format_desc* fmt,
                        boost::posix_time::ptime       start_timecode,
                        int                            audio_channels)
{
    mjpeg_file_header hdr{};
    hdr.magick[0] = 'O'; hdr.magick[1] = 'M';
    hdr.magick[2] = 'A'; hdr.magick[3] = 'V';
    hdr.version        = 2;
    hdr.width          = fmt->width;
    hdr.height         = fmt->height;
    hdr.fps            = fmt->fps;
    hdr.field_mode     = 3; // progressive
    hdr.begin_timecode = start_timecode;

    DWORD written = 0;
#ifdef REPLAY_IO_WINAPI
    WriteFile(idx, &hdr, sizeof(hdr), &written, nullptr);
#else
    fwrite(&hdr, 1, sizeof(hdr), idx);
#endif

    mjpeg_file_header_ex hdr_ex{};
    hdr_ex.video_fourcc[0] = 'm'; hdr_ex.video_fourcc[1] = 'j';
    hdr_ex.video_fourcc[2] = 'p'; hdr_ex.video_fourcc[3] = 'g';
    hdr_ex.audio_fourcc[0] = 'i'; hdr_ex.audio_fourcc[1] = 'n';
    hdr_ex.audio_fourcc[2] = '3'; hdr_ex.audio_fourcc[3] = '2';
    hdr_ex.audio_channels  = audio_channels;

    written = 0;
#ifdef REPLAY_IO_WINAPI
    WriteFile(idx, &hdr_ex, sizeof(hdr_ex), &written, nullptr);
#else
    fwrite(&hdr_ex, 1, sizeof(hdr_ex), idx);
#endif
}

int read_index_header(mjpeg_file_handle idx, mjpeg_file_header** out)
{
    *out = new mjpeg_file_header{};
    DWORD read = 0;
#ifdef REPLAY_IO_WINAPI
    ReadFile(idx, *out, sizeof(mjpeg_file_header), &read, nullptr);
#else
    read = static_cast<DWORD>(fread(*out, 1, sizeof(mjpeg_file_header), idx));
#endif
    if (read != sizeof(mjpeg_file_header)) { delete *out; return 1; }
    return 0;
}

int read_index_header_ex(mjpeg_file_handle idx, mjpeg_file_header_ex** out)
{
    *out = new mjpeg_file_header_ex{};
    DWORD read = 0;
#ifdef REPLAY_IO_WINAPI
    ReadFile(idx, *out, sizeof(mjpeg_file_header_ex), &read, nullptr);
#else
    read = static_cast<DWORD>(fread(*out, 1, sizeof(mjpeg_file_header_ex), idx));
#endif
    if (read != sizeof(mjpeg_file_header_ex)) { delete *out; return 1; }
    return 0;
}

// ── Index entry read / write / seek ──────────────────────────────────────────

void write_index(mjpeg_file_handle idx, long long offset)
{
    DWORD written = 0;
#ifdef REPLAY_IO_WINAPI
    WriteFile(idx, &offset, sizeof(long long), &written, nullptr);
#else
    fwrite(&offset, 1, sizeof(long long), idx);
#endif
}

long long read_index(mjpeg_file_handle idx)
{
    long long offset = 0;
    DWORD read = 0;
#ifdef REPLAY_IO_WINAPI
    ReadFile(idx, &offset, sizeof(long long), &read, nullptr);
#else
    read = static_cast<DWORD>(fread(&offset, 1, sizeof(long long), idx));
#endif
    return (read > 0) ? offset : -1LL;
}

static constexpr long long INDEX_DATA_OFFSET =
    sizeof(mjpeg_file_header) + sizeof(mjpeg_file_header_ex);

int seek_index(mjpeg_file_handle idx, long long frame, uint32_t origin)
{
#ifdef REPLAY_IO_WINAPI
    LARGE_INTEGER pos;
    DWORD         whence;
    if (origin == FILE_CURRENT)
    {
        pos.QuadPart = frame * static_cast<long long>(sizeof(long long));
        whence = FILE_CURRENT;
    }
    else // FILE_BEGIN
    {
        pos.QuadPart = frame * static_cast<long long>(sizeof(long long)) + INDEX_DATA_OFFSET;
        whence = FILE_BEGIN;
    }
    return SetFilePointerEx(idx, pos, nullptr, whence) ? 0 : 1;
#else
    if (origin == FILE_CURRENT)
        return fseek64(idx, frame * sizeof(long long), SEEK_CUR);
    return fseek64(idx, frame * sizeof(long long) + INDEX_DATA_OFFSET, SEEK_SET);
#endif
}

long long tell_index(mjpeg_file_handle idx)
{
#ifdef REPLAY_IO_WINAPI
    LARGE_INTEGER zero{}, pos{};
    SetFilePointerEx(idx, zero, &pos, FILE_CURRENT);
    return (pos.QuadPart - INDEX_DATA_OFFSET) / sizeof(long long);
#else
    return (ftell64(idx) - INDEX_DATA_OFFSET) / sizeof(long long);
#endif
}

long long length_index(mjpeg_file_handle idx)
{
#ifdef REPLAY_IO_WINAPI
    LARGE_INTEGER size{};
    GetFileSizeEx(idx, &size);
    return (size.QuadPart - INDEX_DATA_OFFSET) / sizeof(long long);
#else
    long long cur = ftell64(idx);
    fseek64(idx, 0, SEEK_END);
    long long len = (ftell64(idx) - INDEX_DATA_OFFSET) / sizeof(long long);
    fseek64(idx, cur, SEEK_SET);
    return len;
#endif
}

// ── Frame (MAV data) read / write / seek ─────────────────────────────────────

long long tell_frame(mjpeg_file_handle infile)
{
#ifdef REPLAY_IO_WINAPI
    LARGE_INTEGER zero{}, pos{};
    SetFilePointerEx(infile, zero, &pos, FILE_CURRENT);
    return pos.QuadPart;
#else
    return ftell64(infile);
#endif
}

int seek_frame(mjpeg_file_handle infile, long long offset, uint32_t /*origin*/)
{
#ifdef REPLAY_IO_WINAPI
    LARGE_INTEGER pos;
    pos.QuadPart = offset;
    return SetFilePointerEx(infile, pos, nullptr, FILE_BEGIN) ? 0 : 1;
#else
    return fseek64(infile, offset, SEEK_SET);
#endif
}

// ── JPEG source manager (read) ────────────────────────────────────────────────

static void init_source(j_decompress_ptr cinfo)
{
    reinterpret_cast<src_ptr>(cinfo->src)->start_of_file = TRUE;
}

static boolean fill_input_buffer(j_decompress_ptr cinfo)
{
    auto* src = reinterpret_cast<src_ptr>(cinfo->src);
    DWORD nbytes = 0;
#ifdef REPLAY_IO_WINAPI
    ReadFile(src->infile, src->buffer, VIDEO_INPUT_BUF_SIZE, &nbytes, nullptr);
#else
    nbytes = static_cast<DWORD>(fread(src->buffer, 1, VIDEO_INPUT_BUF_SIZE, src->infile));
#endif
    if (nbytes == 0)
    {
        if (src->start_of_file)
            ERREXIT(cinfo, JERR_INPUT_EMPTY);
        WARNMS(cinfo, JWRN_JPEG_EOF);
        src->buffer[0] = 0xFF;
        src->buffer[1] = JPEG_EOI;
        nbytes = 2;
    }
    src->pub.next_input_byte = src->buffer;
    src->pub.bytes_in_buffer = nbytes;
    src->start_of_file       = FALSE;
    return TRUE;
}

static void skip_input_data(j_decompress_ptr cinfo, long num_bytes)
{
    auto* src = reinterpret_cast<src_ptr>(cinfo->src);
    if (num_bytes > 0)
    {
        while (num_bytes > static_cast<long>(src->pub.bytes_in_buffer))
        {
            num_bytes -= static_cast<long>(src->pub.bytes_in_buffer);
            fill_input_buffer(cinfo);
        }
        src->pub.next_input_byte += num_bytes;
        src->pub.bytes_in_buffer -= num_bytes;
    }
}

static void term_source(j_decompress_ptr) {}

static void jpeg_replay_src(j_decompress_ptr cinfo, mjpeg_file_handle file)
{
    if (!cinfo->src)
    {
        cinfo->src = reinterpret_cast<jpeg_source_mgr*>(
            (*cinfo->mem->alloc_small)(
                reinterpret_cast<j_common_ptr>(cinfo),
                JPOOL_PERMANENT, sizeof(tag_src_mgr)));

        auto* src = reinterpret_cast<src_ptr>(cinfo->src);
        src->buffer = reinterpret_cast<JOCTET*>(
            (*cinfo->mem->alloc_small)(
                reinterpret_cast<j_common_ptr>(cinfo),
                JPOOL_PERMANENT, VIDEO_INPUT_BUF_SIZE * sizeof(JOCTET)));
    }
    auto* src = reinterpret_cast<src_ptr>(cinfo->src);
    src->pub.init_source       = init_source;
    src->pub.fill_input_buffer = fill_input_buffer;
    src->pub.skip_input_data   = skip_input_data;
    src->pub.resync_to_restart = jpeg_resync_to_restart;
    src->pub.term_source       = term_source;
    src->infile                = file;
    src->pub.bytes_in_buffer   = 0;
    src->pub.next_input_byte   = nullptr;
}

// ── JPEG destination manager (write) ─────────────────────────────────────────

static void init_destination(j_compress_ptr cinfo)
{
    auto* dest = reinterpret_cast<dest_ptr>(cinfo->dest);
    dest->buffer = reinterpret_cast<JOCTET*>(
        (*cinfo->mem->alloc_small)(
            reinterpret_cast<j_common_ptr>(cinfo),
            JPOOL_IMAGE, VIDEO_OUTPUT_BUF_SIZE * sizeof(JOCTET)));
    dest->pub.next_output_byte = dest->buffer;
    dest->pub.free_in_buffer   = VIDEO_OUTPUT_BUF_SIZE;
}

static boolean empty_output_buffer(j_compress_ptr cinfo)
{
    auto*  dest    = reinterpret_cast<dest_ptr>(cinfo->dest);
    DWORD  written = 0;
    bool   ok      = false;
#ifdef REPLAY_IO_WINAPI
    ok = WriteFile(dest->outfile, dest->buffer, VIDEO_OUTPUT_BUF_SIZE, &written, nullptr);
#else
    written = static_cast<DWORD>(fwrite(dest->buffer, 1, VIDEO_OUTPUT_BUF_SIZE, dest->outfile));
    ok = (written > 0);
#endif
    if (!ok) ERREXIT(cinfo, JERR_FILE_WRITE);
    dest->pub.next_output_byte = dest->buffer;
    dest->pub.free_in_buffer   = VIDEO_OUTPUT_BUF_SIZE;
    return TRUE;
}

static void term_destination(j_compress_ptr cinfo)
{
    auto*    dest      = reinterpret_cast<dest_ptr>(cinfo->dest);
    uint32_t datacount = VIDEO_OUTPUT_BUF_SIZE - static_cast<uint32_t>(dest->pub.free_in_buffer);
    if (datacount > 0)
    {
        DWORD written = 0;
        bool  ok      = false;
#ifdef REPLAY_IO_WINAPI
        ok = WriteFile(dest->outfile, dest->buffer, datacount, &written, nullptr);
#else
        written = static_cast<DWORD>(fwrite(dest->buffer, 1, datacount, dest->outfile));
        ok = (written > 0);
#endif
        if (!ok) ERREXIT(cinfo, JERR_FILE_WRITE);
    }
}

static void jpeg_replay_dest(j_compress_ptr cinfo, mjpeg_file_handle file)
{
    if (!cinfo->dest)
    {
        cinfo->dest = reinterpret_cast<jpeg_destination_mgr*>(
            (*cinfo->mem->alloc_small)(
                reinterpret_cast<j_common_ptr>(cinfo),
                JPOOL_PERMANENT, sizeof(tag_dest_mgr)));
    }
    auto* dest = reinterpret_cast<dest_ptr>(cinfo->dest);
    dest->pub.init_destination    = init_destination;
    dest->pub.empty_output_buffer = empty_output_buffer;
    dest->pub.term_destination    = term_destination;
    dest->outfile                 = file;
}

// ── Error handler ─────────────────────────────────────────────────────────────

static void replay_jpeg_error_exit(j_common_ptr cinfo)
{
    (*cinfo->err->output_message)(cinfo);
    longjmp(reinterpret_cast<error_mgr*>(cinfo->err)->setjmp_buffer, 1);
}

// ── Public: read_frame ────────────────────────────────────────────────────────

uint32_t read_frame(mjpeg_file_handle infile,
                    uint32_t*  width,
                    uint32_t*  height,
                    uint8_t**  image,
                    uint32_t*  audio_size,
                    int32_t**  audio)
{
    // Read audio block first
    uint32_t audio_buf_size = 0;
    DWORD    read           = 0;
#ifdef REPLAY_IO_WINAPI
    ReadFile(infile, &audio_buf_size, sizeof(uint32_t), &read, FALSE);
#else
    read = static_cast<DWORD>(fread(&audio_buf_size, 1, sizeof(uint32_t), infile));
#endif

    *audio      = nullptr;
    *audio_size = audio_buf_size;

    if (audio_buf_size > 0)
    {
        *audio = new int32_t[audio_buf_size / 4];
        read   = 0;
#ifdef REPLAY_IO_WINAPI
        ReadFile(infile, *audio, static_cast<DWORD>(audio_buf_size), &read, FALSE);
#else
        fread(*audio, 1, audio_buf_size, infile);
#endif
    }

    // Decode JPEG
    jpeg_decompress_struct cinfo{};
    error_mgr              jerr{};

    cinfo.err             = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit   = replay_jpeg_error_exit;

#pragma warning(disable:4611)
    if (setjmp(jerr.setjmp_buffer))
    {
        jpeg_destroy_decompress(&cinfo);
        return 0;
    }
#pragma warning(default:4611)

    jpeg_create_decompress(&cinfo);
    jpeg_replay_src(&cinfo, infile);
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);

    int row_stride = cinfo.output_width * 3;
    *width  = cinfo.output_width;
    *height = cinfo.output_height;
    *image  = new uint8_t[(*width) * (*height) * 3];

    JSAMPROW row[1];
    while (cinfo.output_scanline < cinfo.output_height)
    {
        row[0] = reinterpret_cast<JSAMPROW>((*image) + cinfo.output_scanline * row_stride);
        jpeg_read_scanlines(&cinfo, row, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    return (*width) * (*height) * 3;
}

// ── Public: write_frame ───────────────────────────────────────────────────────

long long write_frame(mjpeg_file_handle  outfile,
                      uint32_t           width,
                      uint32_t           height,
                      const uint8_t*     image,
                      short              quality,
                      mjpeg_process_mode mode,
                      chroma_subsampling subsampling,
                      const int32_t*     audio_data,
                      uint32_t           audio_data_length)
{
    long long start_pos = tell_frame(outfile);

    // Write audio block
    DWORD written = 0;
#ifdef REPLAY_IO_WINAPI
    WriteFile(outfile, &audio_data_length, sizeof(uint32_t), &written, nullptr);
    if (audio_data_length > 0)
        WriteFile(outfile, audio_data, audio_data_length, &written, nullptr);
#else
    fwrite(&audio_data_length, 1, sizeof(uint32_t), outfile);
    if (audio_data_length > 0)
        fwrite(audio_data, 1, audio_data_length, outfile);
#endif

    // JPEG encode
    jpeg_compress_struct cinfo{};
    jpeg_error_mgr       jerr{};

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_replay_dest(&cinfo, outfile);

    cinfo.image_width      = width;
    cinfo.image_height     = height;
    cinfo.input_components = 4;
    cinfo.in_color_space   = JCS_EXT_BGRX; // CasparCG 2.5 native pixel format

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    // Chroma subsampling
    auto set_samp = [&](int y_h, int y_v)
    {
        cinfo.comp_info[0].h_samp_factor = y_h;
        cinfo.comp_info[0].v_samp_factor = y_v;
        cinfo.comp_info[1].h_samp_factor = 1;
        cinfo.comp_info[1].v_samp_factor = 1;
        cinfo.comp_info[2].h_samp_factor = 1;
        cinfo.comp_info[2].v_samp_factor = 1;
    };

    switch (subsampling)
    {
        case chroma_subsampling::Y444: set_samp(1, 1); break;
        case chroma_subsampling::Y422: set_samp(2, 1); break;
        case chroma_subsampling::Y420: set_samp(2, 2); break;
        case chroma_subsampling::Y411: set_samp(4, 1); break;
    }

    jpeg_start_compress(&cinfo, TRUE);

    uint32_t row_stride = width * 4;
    JSAMPROW row[1];

    while (cinfo.next_scanline < cinfo.image_height)
    {
        uint32_t src_line = 0;
        if (mode == mjpeg_process_mode::PROGRESSIVE)
            src_line = cinfo.next_scanline;
        else if (mode == mjpeg_process_mode::UPPER)
            src_line = cinfo.next_scanline * 2;
        else // LOWER
            src_line = cinfo.next_scanline * 2 + 1;

        row[0] = reinterpret_cast<JSAMPROW>(image + src_line * row_stride);
        jpeg_write_scanlines(&cinfo, row, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    return start_pos;
}

#pragma warning(default:4267)

}} // namespace caspar::replay

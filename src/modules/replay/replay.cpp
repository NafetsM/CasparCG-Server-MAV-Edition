/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
* Copyright (c) 2013 Technical University of Lodz Multimedia Centre <office@cm.p.lodz.pl>
*
* Ported to CasparCG 2.5
*/

#include "replay.h"

#include "producer/replay_producer.h"
#include "consumer/replay_consumer.h"

#include <setjmp.h>
#include <jpeglib.h>

#include <core/producer/frame_producer.h>
#include <core/consumer/frame_consumer.h>
#include <core/module_dependencies.h>
#include <core/frame/draw_frame.h>

#include <common/log.h>

namespace caspar { namespace replay {

#ifndef JCS_EXTENSIONS
#pragma message("Building against non-turbo libjpeg")
#define JCS_EXT_RGB 6
#endif

namespace {

int  jpeg_version  = -1;
bool jpeg_is_turbo = false;

void jpeg_version_error_handler(j_common_ptr cinfo)
{
    jpeg_version = cinfo->err->msg_parm.i[0];
}

std::wstring detect_libjpeg_version()
{
    jpeg_compress_struct cinfo{};
    jpeg_error_mgr       error_mgr{};

    error_mgr.error_exit = &jpeg_version_error_handler;
    cinfo.err            = &error_mgr;

    jpeg_create_compress(&cinfo);
    cinfo.input_components = 3;
    jpeg_set_defaults(&cinfo);
    cinfo.in_color_space = static_cast<J_COLOR_SPACE>(JCS_EXT_RGB);

    try
    {
        jpeg_default_colorspace(&cinfo);
        if (jpeg_version == -1)
        {
            jpeg_is_turbo = true;
            jpeg_destroy_compress(&cinfo);
        }
    }
    catch (...)
    {
        CASPAR_LOG(error) << L"[replay] JPEG lib test exception";
    }

    // Force version detection by passing version = -1
    jpeg_CreateCompress(&cinfo, -1, sizeof(cinfo));

    std::wstringstream str;
    str << L"libjpeg" << (jpeg_is_turbo ? L"-turbo " : L" ") << jpeg_version;
    return str.str();
}

} // anonymous namespace

void init(core::module_dependencies dependencies)
{
    dependencies.consumer_registry->register_consumer_factory(
        L"Replay Consumer", create_consumer);

    dependencies.producer_registry->register_producer_factory(
        L"Replay Producer", create_producer);

    CASPAR_LOG(info) << L"[replay] JPEG lib version: " << detect_libjpeg_version();
}

}} // namespace caspar::replay

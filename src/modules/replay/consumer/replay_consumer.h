/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
* Copyright (c) 2013 Technical University of Lodz Multimedia Centre <office@cm.p.lodz.pl>
*
* Ported to CasparCG 2.5
*/

#pragma once

#include <common/memory.h>
#include <core/fwd.h>
#include <core/consumer/frame_consumer.h>

#include <vector>
#include <string>

namespace caspar { namespace replay {

spl::shared_ptr<core::frame_consumer> create_consumer(
    const std::vector<std::wstring>&                   params,
    std::vector<spl::shared_ptr<core::video_channel>>  channels,
    const core::video_format_repository&               format_repository);

}} // namespace caspar::replay

/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
* Copyright (c) 2013 Technical University of Lodz Multimedia Centre <office@cm.p.lodz.pl>
*
* Ported to CasparCG 2.5
*/

#pragma once

#include <core/producer/frame_producer.h>

#define REPLAY_PRODUCER_BUFFER_SIZE 3

namespace caspar { namespace replay {

spl::shared_ptr<core::frame_producer> create_producer(
    const core::frame_producer_dependencies& dependencies,
    const std::vector<std::wstring>&         params);

core::draw_frame create_thumbnail(
    const core::frame_producer_dependencies& dependencies,
    const std::wstring&                      media_file);

}} // namespace caspar::replay

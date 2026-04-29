cmake_minimum_required(VERSION 3.16)

# ── libjpeg-turbo ─────────────────────────────────────────────────────────────
find_package(JPEG REQUIRED)

# ── Sources ───────────────────────────────────────────────────────────────────
set(SOURCES
    consumer/replay_consumer.cpp
    consumer/replay_consumer.h
    producer/replay_producer.cpp
    producer/replay_producer.h
    util/file_operations.cpp
    util/file_operations.h
    util/frame_operations.cpp
    util/frame_operations.h
    replay.cpp
    replay.h
)

# ── CasparCG 2.5 module registration ─────────────────────────────────────────
# This macro: creates the library, links common+core, sets up include paths,
# AND registers the module in the auto-generated included_modules.h
casparcg_add_module_project(replay
    SOURCES         ${SOURCES}
    INIT_FUNCTION   caspar::replay::init
    NAME            "Replay (MAV)"
)

# ── libjpeg link ──────────────────────────────────────────────────────────────
target_link_libraries(replay PRIVATE ${JPEG_LIBRARIES})
target_include_directories(replay PRIVATE ${JPEG_INCLUDE_DIRS})

# ── IDE organisation ──────────────────────────────────────────────────────────
set_target_properties(replay PROPERTIES FOLDER modules)
source_group("sources"           FILES replay.h replay.cpp)
source_group("sources\\consumer" REGULAR_EXPRESSION "consumer/.*")
source_group("sources\\producer" REGULAR_EXPRESSION "producer/.*")
source_group("sources\\util"     REGULAR_EXPRESSION "util/.*")

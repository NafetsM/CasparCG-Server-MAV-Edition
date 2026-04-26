cmake_minimum_required(VERSION 3.16)
project(replay)

# ── libjpeg-turbo ─────────────────────────────────────────────────────────────
# CasparCG 2.5 uses vcpkg, so find_package works for libjpeg-turbo.
# Fallback: manual path via PACKAGES_FOLDER (legacy CasparCG convention).

find_package(JPEG QUIET)

if(NOT JPEG_FOUND)
    # Legacy fallback — same structure as original fork
    if(MSVC)
        set(_JPEG_INC  "${PACKAGES_FOLDER}/libjpeg-turbo/include/win32")
        set(_JPEG_LIB  "${PACKAGES_FOLDER}/libjpeg-turbo/lib/win32/jpeg.lib")
        set(_JPEG_DLL  "${PACKAGES_FOLDER}/libjpeg-turbo/bin/win32/jpeg62.dll")
    else()
        set(_JPEG_INC  "${PACKAGES_FOLDER}/libjpeg-turbo/include/linux")
        set(_JPEG_LIB  "jpeg")
        set(_JPEG_DLL  "${PACKAGES_FOLDER}/libjpeg-turbo/bin/linux/libjpeg.so.62")
    endif()
    casparcg_add_runtime_dependency("${_JPEG_DLL}")
endif()

# ── Sources ───────────────────────────────────────────────────────────────────
set(SOURCES
    consumer/replay_consumer.cpp
    producer/replay_producer.cpp
    util/file_operations.cpp
    util/frame_operations.cpp
    replay.cpp
)

set(HEADERS
    consumer/replay_consumer.h
    producer/replay_producer.h
    util/file_operations.h
    util/frame_operations.h
    replay.h
)

# ── Library target ────────────────────────────────────────────────────────────
add_library(replay ${SOURCES} ${HEADERS})

target_compile_features(replay PUBLIC cxx_std_17)

# ── Include paths ──────────────────────────────────────────────────────────────
target_include_directories(replay
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..   # src/ root
        ${CMAKE_CURRENT_SOURCE_DIR}/..      # modules/ root
)

if(JPEG_FOUND)
    target_include_directories(replay PRIVATE ${JPEG_INCLUDE_DIRS})
else()
    target_include_directories(replay PRIVATE ${_JPEG_INC})
endif()

# ── Link libraries ────────────────────────────────────────────────────────────
target_link_libraries(replay
    PRIVATE
        common
        core
        $<IF:$<BOOL:${JPEG_FOUND}>,${JPEG_LIBRARIES},${_JPEG_LIB}>
)

# ── IDE organisation ──────────────────────────────────────────────────────────
set_target_properties(replay PROPERTIES FOLDER modules)
source_group("sources"          FILES replay.h replay.cpp)
source_group("sources\\consumer" REGULAR_EXPRESSION "consumer/.*")
source_group("sources\\producer" REGULAR_EXPRESSION "producer/.*")
source_group("sources\\util"     REGULAR_EXPRESSION "util/.*")

# ── CasparCG module registration macros ───────────────────────────────────────
casparcg_add_include_statement("modules/replay/replay.h")
casparcg_add_init_statement("replay::init" "replay")
casparcg_add_module_project("replay")

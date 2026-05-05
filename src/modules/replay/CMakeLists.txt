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
    util/jpeg_codec.h
    util/jpeg_codec_cpu.cpp
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

# ── optionales nvJPEG ─────────────────────────────────────────────────────────
option(ENABLE_NVJPEG "GPU-beschleunigtes JPEG via NVIDIA nvJPEG" OFF)

if(ENABLE_NVJPEG)
    find_package(CUDAToolkit REQUIRED)
    target_compile_definitions(replay PRIVATE ENABLE_NVJPEG)
    target_link_libraries(replay PRIVATE
        CUDA::cudart
        CUDA::nvjpeg
    )
    target_sources(replay PRIVATE util/jpeg_codec_nvjpeg.cpp)
    message(STATUS "replay: nvJPEG encoder enabled (CUDA ${CUDAToolkit_VERSION})")

    # Copy CUDA runtime DLLs to the shell output directory so the Release folder
    # is self-contained and can be deployed without a CUDA Toolkit on the target machine.
    add_custom_command(TARGET replay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:CUDA::cudart>
            $<TARGET_FILE:CUDA::nvjpeg>
            "${CMAKE_BINARY_DIR}/shell/$<CONFIG>/"
        COMMENT "Copying CUDA runtime DLLs to shell output"
    )
endif()

# ── Compiler flags ────────────────────────────────────────────────────────────
if (NOT MSVC)
    # #pragma warning(...) directives are MSVC-specific; suppress the unknown-pragma
    # warning on GCC/Clang so that -Werror does not turn them into build errors.
    target_compile_options(replay PRIVATE -Wno-unknown-pragmas)
endif()

# ── optional NVOF (NVIDIA Optical Flow frame interpolation) ───────────────────
option(ENABLE_NVOF "Enable NVIDIA Optical Flow interpolation for slow-motion replay" OFF)

if(ENABLE_NVOF)
    find_package(CUDAToolkit REQUIRED)
    target_compile_definitions(replay PRIVATE ENABLE_NVOF)
    target_link_libraries(replay PRIVATE CUDA::cudart)
    target_sources(replay PRIVATE producer/nvof_interpolator.cpp)
    message(STATUS "replay: NVOF interpolation enabled (CUDA ${CUDAToolkit_VERSION})")
endif()

# ── IDE organisation ──────────────────────────────────────────────────────────
set_target_properties(replay PROPERTIES FOLDER modules)
source_group("sources"           FILES replay.h replay.cpp)
source_group("sources\\consumer" REGULAR_EXPRESSION "consumer/.*")
source_group("sources\\producer" REGULAR_EXPRESSION "producer/.*")
source_group("sources\\util"     REGULAR_EXPRESSION "util/.*")

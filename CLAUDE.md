# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

CasparCG Server MAV Edition is a professional broadcast playout server (version 2.6.0 Dev) with an added real-time sport-replay module. The replay module records Motion-JPEG streams with hardware timestamps and supports variable-speed playback and time-based seeking. Development documentation lives in [docs/dev-tasks/](docs/dev-tasks/).

## Building

### Windows (primary development platform)

```powershell
# Configure (from repo root)
cmake -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -B build ../src

# Build
msbuild build/"CasparCG Server.sln" /p:Configuration=Release /m:$env:NUMBER_OF_PROCESSORS
```

Full packaging build (downloads dependencies, creates distributable zip):
```powershell
.\tools\windows\build.bat
```

Optional CMake flags:
- `-DENABLE_NVJPEG=ON` — GPU JPEG encoding via CUDA (requires CUDA Toolkit 13.0+)
- `-DENABLE_HTML=ON` — HTML producer via CEF (default ON)
- `-DENABLE_NVOF=ON` — NVIDIA Optical Flow stub (not yet implemented)

### Linux

```bash
./tools/linux/install-dependencies
sudo add-apt-repository ppa:casparcg/ppa && sudo apt-get install casparcg-cef-142-dev
cmake -B build src && cmake --build build --parallel && cmake --install build --prefix staging
# Or via Docker:
./tools/linux/build-in-docker && ./tools/linux/extract-from-docker
```

## Architecture

### Module System

The server is built around pluggable **channels** (video pipelines) and **modules** (producers/consumers):

- [src/core/](src/core/) — Channel, frame, mixer abstractions. `frame.h` defines the central `frame` object (includes `hardware_timestamp` field added for MAV).
- [src/modules/](src/modules/) — I/O modules: `decklink`, `ffmpeg`, `screen`, `html`, `replay`, `oal`, `newtek`, `artnet`, `bluefish`, `image`, `flash`.
- [src/protocol/](src/protocol/) — AMCP (TCP text protocol) and OSC command handlers.
- [src/shell/](src/shell/) — Server entry point, module registration, config loading.
- [src/accelerator/](src/accelerator/) — GPU abstraction layer (DirectX3D / OpenGL).
- [src/common/](src/common/) — Threading, logging, memory, filesystem utilities.

### Replay Module (`src/modules/replay/`)

The MAV replay module is the core MAV addition:

- **consumer/** — Records BGRA frames → JPEG → `.mav` file with `.idx` index.
- **producer/** — Plays back `.mav` files with variable speed (forward/reverse), frame-level and time-based seeking.
- **util/** — File I/O (`file_operations.h/cpp`), JPEG codec abstraction (`jpeg_codec.h`), CPU encoder (`jpeg_codec_cpu.cpp`), NVIDIA encoder (`jpeg_codec_nvjpeg.cpp`).

#### File Format (v4 — current)

`.mav`: sequential frames as `[audio_size:u32][audio:int32 LE][JPEG data]`

`.idx`: fixed header + 16-byte entries (`index_entry_v4`):
- `int64_t file_offset` — byte position in `.mav`
- `int64_t timestamp_microseconds` — µs since `begin_timecode`; `INT64_MIN` = recording gap

Interlaced material: each index entry = one field (JPEG at half height); two consecutive entries = one full frame.

Version compatibility: v2 (linear only), v3 (seek/speed, no timestamps), v4 (full time-based seek).

#### Key AMCP Commands

```
# Recording
ADD <ch> REPLAY <filename> [QUALITY <q>] [SUBSAMPLING 444|422|420|411]

# Playback
PLAY <ch>-<layer> <filename> [SEEK [|]<n>] [SPEED <s>] [LENGTH <n>] [AUDIO 0|1]

# Runtime control
CALL <ch>-<layer> SPEED <s>
CALL <ch>-<layer> SEEK [+|-||]<n>[ms|s]
CALL <ch>-<layer> SEEK_ABS <utc_ms>
CALL <ch>-<layer> PAUSE
```

`INFO <ch>-<layer>` returns: `file/frame`, `file/time`, `file/live_edge_absolute_ms`, `file/time_behind_live_ms`, `file/gap_detected`.

### Hardware Timestamps (Phase 1+2)

DeckLink producer (`src/modules/decklink/producer/decklink_producer.cpp`) captures hardware timestamps and populates `frame.hardware_timestamp`. The replay consumer writes these into the v4 index. The replay producer uses them for `SEEK_ABS` (UTC-based seeking) and `time_behind_live_ms` calculations.

### NVIDIA GPU Encoding (Phase 2)

`jpeg_codec_nvjpeg.cpp` implements the `jpeg_codec` interface using CUDA nvJPEG. Enabled at compile time with `-DENABLE_NVJPEG=ON`. At runtime the consumer selects it automatically when a CUDA device is available; falls back to CPU (libjpeg-turbo). Required DLLs (`cudart64_13.dll`, `nvjpeg64_13.dll`) are copied to the output directory by CMake.

## Development Notes

- The project uses `boost::shared_ptr` / `boost::signals2` extensively — do not replace with `std::` equivalents without checking compatibility across all modules.
- Frame pipeline is lock-free and latency-sensitive; avoid allocations or blocking calls on the frame-delivery path.
- AMCP command parsing lives in `src/protocol/amcp/`; new commands must be registered in `amcp_command_repository.cpp`.
- DeckLink SDK headers are vendored under `dependencies/` and must not be replaced from an incompatible SDK version.
- The `hardware_timestamp` field is `INT64_MIN` when unavailable (non-DeckLink sources); all consumers/producers must guard against this sentinel.

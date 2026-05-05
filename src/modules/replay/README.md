# Replay Module (MAV)

Sport-replay module for CasparCG 2.5. Records a channel as a Motion-JPEG stream
(`.mav` + `.idx`) and plays it back at variable speed, forward and backward —
without stopping the recording (live replay).

Ported from the original CasparCG 2.0/2.2 replay module by TU Lodz and
adapted to the CasparCG 2.5 architecture (BGRA, updated `frame_consumer`/`frame_producer` signatures).

## File Format

A recording consists of two files in the `media/` folder:

| File             | Contents                                                       |
| ---------------- | -------------------------------------------------------------- |
| `<name>.mav`     | Sequence of `[audio_size:u32][audio:int32 LE][JPEG]` per entry |
| `<name>.idx`     | Header + 16-byte `index_entry_v4` per entry (see below)        |

**Current version:** 4 (as of 2026-05-05, branch `feature/timecode`)

Header layout (`mjpeg_file_header` + `mjpeg_file_header_ex`):

```
magick[4]            = 'OMAV'
version              = 4
width, height        = full frame size (also for interlaced)
fps                  = fields/s (interlaced) or frames/s (progressive)
field_mode           = 1 (lower-first), 2 (upper-first), 3 (progressive)
begin_timecode       = UTC time at recording start
video_fourcc[4]      = 'mjpg'
audio_fourcc[4]      = 'in32'
audio_channels       = e.g. 16
audio_sample_rate    = Hz, e.g. 48000
```

Index entry layout (`index_entry_v4`, 16 bytes):

```
file_offset            int64_t   byte offset of this entry in .mav
timestamp_microseconds int64_t   µs since begin_timecode; INT64_MIN = gap
```

The `INDEX_DATA_OFFSET` (start of index data) equals
`sizeof(mjpeg_file_header) + sizeof(mjpeg_file_header_ex)` and is unchanged from v3.
The producer auto-detects the index version (`version < 4` → 8-byte v3 stride, `version >= 4` → 16-byte v4 stride).

For **interlaced** recordings each MAV entry contains exactly **one field**
(JPEG at half height, e.g. 1920×540 for 1080i50). Two consecutive entries make one full frame.
For **progressive** recordings each entry is a full frame.

## AMCP Commands

### Recording — Consumer

```text
ADD <channel> REPLAY <filename> [QUALITY <q>] [SUBSAMPLING 444|422|420|411]
REMOVE <channel> <consumer-index>
```

| Parameter      | Default | Description                                             |
| -------------- | ------- | ------------------------------------------------------- |
| `<filename>`   | —       | Base name (without `.mav`/`.idx`), stored under `media/`. **Required** — the command is rejected if omitted. |
| `QUALITY`      | 90      | JPEG quality 1–100                                      |
| `SUBSAMPLING`  | 422     | Chroma subsampling                                      |

> **Note:** `<filename>` is mandatory. `ADD <channel> REPLAY` without a filename
> is silently ignored — no consumer is created.

Field order and sample rate are taken automatically from the channel's `video_format_desc`.
For interlaced channels (e.g. 1080i50) the consumer stores both fields individually
and preserves the audio data of each field.

**Examples**

```text
ADD 1 REPLAY TEST3                              # standard
ADD 1 REPLAY MATCH SUBSAMPLING 444 QUALITY 95   # high quality
REMOVE 1 150                                    # index 150 = replay consumer
```

### Playback — Producer

```text
PLAY <channel>-<layer> <filename> [SEEK [|]<n>] [SPEED <s>] [LENGTH <n>] [AUDIO 0|1]
```

| Parameter | Default           | Description                                                         |
| --------- | ----------------- | ------------------------------------------------------------------- |
| `SEEK`    | `0` (= beginning) | Frame position. `\|<n>` = `<n>` frames before the current live end. |
| `SPEED`   | `1.0`             | Playback speed. Negative = reverse. `0` = paused.                   |
| `LENGTH`  | `0` (= unlimited) | Maximum number of frames to play                                    |
| `AUDIO`   | auto              | `1` = audio on, `0` = muted. Default: on if the file has audio channels. |

**Examples**

```text
PLAY 1-1 TEST3                          # audio on automatically if present
PLAY 1-1 TEST3 SPEED 0.25               # quarter-speed slow motion
PLAY 1-1 TEST3 SPEED -1                 # reverse playback
PLAY 1-1 TEST3 SEEK |100                # start 100 frames before live end
PLAY 1-1 TEST3 LENGTH 250 AUDIO 0       # 10 seconds at 25 fps, muted
```

### Control During Playback — `CALL`

```text
CALL <channel>-<layer> SPEED <s>
CALL <channel>-<layer> PAUSE
CALL <channel>-<layer> SEEK [+|-||]<n>[ms|s]
CALL <channel>-<layer> SEEK_ABS <utc_ms>
CALL <channel>-<layer> LENGTH <n>
CALL <channel>-<layer> AUDIO 0|1
```

| Command                  | Effect                                                                       |
| ------------------------ | ---------------------------------------------------------------------------- |
| `SPEED <s>`              | Change speed (same semantics as `PLAY`)                                      |
| `PAUSE`                  | Set speed to `0`                                                             |
| `SEEK <n>`               | Absolute position (frame `n`)                                                |
| `SEEK +<n>`              | `n` frames forward                                                           |
| `SEEK -<n>`              | `n` frames backward                                                          |
| `SEEK \|<n>`             | `n` frames before the current live end                                       |
| `SEEK \|<n>s`            | `n` seconds before the current live end (**v4 files only**)                  |
| `SEEK \|<n>ms`           | `n` milliseconds before the current live end (**v4 files only**)             |
| `SEEK_ABS <utc_ms>`      | Seek to the frame nearest to an absolute UTC timestamp in milliseconds (**v4 files only**) — use `file/live_edge_absolute_ms` from `INFO` to compute the target |
| `LENGTH <n>`             | Change maximum playback length (`0` = unlimited)                             |
| `AUDIO 0` / `AUDIO 1`   | Mute / unmute audio during playback                                          |

**Time-based seek examples:**

```text
CALL 1-1 SEEK |3s          # 3 seconds before live edge
CALL 1-1 SEEK |1500ms      # 1.5 seconds before live edge
CALL 1-1 SEEK_ABS 1746360225000   # absolute UTC timestamp (ms since epoch)
```

**Multi-channel synchronisation workflow:**

```text
# 1. Query reference channel for live-edge UTC timestamp
INFO 1 10      → file.live_edge_absolute_ms = 1746360225000

# 2. Send all channels to the same absolute timestamp
CALL 1-10 SEEK_ABS 1746360224000   # 1 second before live
CALL 1-11 SEEK_ABS 1746360224000
CALL 2-10 SEEK_ABS 1746360224000
```

### Producer State Values

Readable via `INFO <channel> <layer>` (returns XML `201 INFO OK`).
Keys use `.` as separator in the XML body.

| State key                       | Type    | Description                                                        |
| ------------------------------- | ------- | ------------------------------------------------------------------ |
| `file/frame`                    | int × 2 | Current frame index / total frames in file                         |
| `file/time`                     | f × 2   | Current position in seconds / total duration in seconds            |
| `file/fps`                      | float   | Recording frame rate                                               |
| `file/speed`                    | float   | Current playback speed                                             |
| `file/path`                     | string  | File path (without extension)                                      |
| `file/live_edge_absolute_ms`    | int64   | UTC timestamp of the most recent frame in the recording (ms since epoch). **v4 only.** |
| `file/time_behind_live_ms`      | int64   | How far behind the live edge the current playback position is (ms). **v4 only.** |
| `file/gap_detected`             | bool    | `true` if the current frame has `timestamp_microseconds == INT64_MIN` (recording gap). **v4 only.** |

## Live Replay Workflow

```text
# 1) Start recording (e.g. on channel 1)
ADD 1 REPLAY MATCH

# 2) Play back on a second channel or layer
PLAY 2-1 MATCH SPEED 0.5 SEEK |200

# 3) Jump to a different position during playback
CALL 2-1 SEEK |400

# 4) Stop recording
REMOVE 1 150
```

The producer actively waits for the `.idx` file to grow, so it can already
seek into the past while recording is still in progress.

## Testing Interlaced

A real interlaced source is not required — it is sufficient to configure the
**CasparCG channel itself** to an interlaced mode. The channel mixer
deinterlaces/interlaces internally so the replay consumer sees `field::a`/`field::b`
regardless of whether the source is progressive (NDI/OBS, FFmpeg file, AMB Loop, …).

```xml
<channel>
    <video-mode>1080i5000</video-mode>
    <consumers>
        <screen />
        <system-audio />
    </consumers>
</channel>
```

The replay consumer log should then show:

```
replay_consumer[…] Recording 1920x1080 interlaced (UFF) @ 50 fps, 16ch @ 48000 Hz
```

## GPU-Accelerated JPEG Encoding (nvJPEG, `ENABLE_NVJPEG`)

The consumer supports optional hardware-accelerated JPEG encoding via NVIDIA nvJPEG.
Enable it at CMake configure time:

```powershell
cmake -DENABLE_NVJPEG=ON `
      -DCUDAToolkit_ROOT="C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.2" `
      <build-dir>
```

When active, `replay_consumer` instantiates an `nvjpeg_encoder_impl` instead of
the default `cpu_encoder_impl`. Both implement the same `jpeg_encoder` interface
(`util/jpeg_codec.h`). If `nvjpegCreate()` fails at runtime (no CUDA device),
the consumer falls back to the CPU encoder automatically.

**Expected throughput** (RTX 3070, quality 90, Y422): ~4–6× 1080p50 streams,
compared to ~3× 1080i50 on CPU (libjpeg-turbo).

### Deployment

The build copies the required CUDA runtime DLLs automatically into the output
folder (`build/shell/Release/`) so that the target machine does not need a
CUDA Toolkit — only a compatible NVIDIA driver is required:

| DLL | Source |
|---|---|
| `cudart64_13.dll` | CUDA Runtime |
| `nvjpeg64_13.dll` | nvJPEG library |

### Log output on startup of a consumer

```
[info]  nvJPEG encoder initialized on GPU: NVIDIA GeForce RTX 3070 (CUDA compute 8.6), quality=90
[info]  replay_consumer: JPEG encoder = nvJPEG (GPU)
```

---

## Known Limitations

- A sample-rate mismatch between recording and playback channel causes incorrect
  audio pitch. The sample rate is stored in the header and logged on open.
- Field-order heuristic in the consumer: HD interlaced (≥720 lines) → upper-field-first,
  SD interlaced → lower-field-first. Custom formats with a different field order
  may need to be added here.
- File format version 1 (CasparCG ≤ 2.0) is no longer supported.
- **Version 2 (before sample-rate extension)**: partially readable.
  - **Linear playback (`SPEED 1`) works** — the index stream is read sequentially.
  - **`SEEK`, `SPEED ≠ 1`, pause/resume, and reverse playback do not work**
    (v2 has a 12-byte extended header vs. 16 bytes assumed by `INDEX_DATA_OFFSET`).
  - Sample rate falls back to 48000 Hz.
  - Recommendation: re-record with the current code once the file is needed for
    anything other than plain linear playback. A warning is logged on open.
- **Version 3 files** (8-byte index entries): fully supported for all seeking and
  speed modes. Time-based `SEEK |Ns`/`|Nms` and `SEEK_ABS` are silently ignored
  (no timestamps available); frame-based seeking continues to work normally.
- **Hardware timestamp gaps** (`INT64_MIN`): if the Decklink producer was not the
  source of a frame (e.g. FFmpeg file input), `hardware_timestamp` is `-1` and the
  index entry is written with `INT64_MIN`. Time-based seeking skips such entries
  and lands on the next frame with a valid timestamp.
- **NVOF frame interpolation** (Phase 3): the interface is defined in
  `producer/nvof_interpolator.h` but the implementation (`nvof_interpolator.cpp`)
  has not been written yet. Slow-motion currently uses the existing blend-based
  fallback.

## GPU Acceleration Options

| CMake option       | Default | Description                                                     |
| ------------------ | ------- | --------------------------------------------------------------- |
| `ENABLE_NVJPEG`    | `OFF`   | Hardware JPEG encoding via NVIDIA nvJPEG (consumer)             |
| `ENABLE_NVOF`      | `OFF`   | NVIDIA Optical Flow frame interpolation for slow-motion (producer, stub — not yet implemented) |

Both options require `CUDAToolkit` and a compatible NVIDIA driver. See the nvJPEG section above for deployment details.

## Source Files

- [replay.cpp](replay.cpp) – module registration, libjpeg version detection
- [consumer/replay_consumer.cpp](consumer/replay_consumer.cpp) – recording; writes v4 index entries with hardware timestamps
- [producer/replay_producer.cpp](producer/replay_producer.cpp) – playback; v4/v3-aware index dispatch, time-based seeking, extended diagnostics
- [producer/nvof_interpolator.h](producer/nvof_interpolator.h) – NVOF interpolator interface stub (implementation pending)
- [util/file_operations.{h,cpp}](util/file_operations.h) – `.mav`/`.idx` format; v3 and v4 I/O functions, `read_timestamp_at()`
- [util/frame_operations.{h,cpp}](util/frame_operations.h) – field/frame interlacing, blending
- [util/jpeg_codec.h](util/jpeg_codec.h) – abstract `jpeg_encoder` interface
- [util/jpeg_codec_cpu.cpp](util/jpeg_codec_cpu.cpp) – CPU encoder (libjpeg-turbo, always available)
- [util/jpeg_codec_nvjpeg.cpp](util/jpeg_codec_nvjpeg.cpp) – GPU encoder (nvJPEG, compiled only with `ENABLE_NVJPEG`)

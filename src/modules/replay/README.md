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
| `<name>.idx`     | Header + 64-bit offset of each entry in `.mav`                 |

**Current version:** 3 (as of 2026-04-28)

Header layout (`mjpeg_file_header` + `mjpeg_file_header_ex`):

```
magick[4]            = 'OMAV'
version              = 3
width, height        = full frame size (also for interlaced)
fps                  = fields/s (interlaced) or frames/s (progressive)
field_mode           = 1 (lower-first), 2 (upper-first), 3 (progressive)
begin_timecode       = UTC time at start
video_fourcc[4]      = 'mjpg'
audio_fourcc[4]      = 'in32'
audio_channels       = e.g. 16
audio_sample_rate    = Hz, e.g. 48000   (new in v3)
```

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
CALL <channel>-<layer> SEEK [+|-||]<n>
CALL <channel>-<layer> LENGTH <n>
CALL <channel>-<layer> AUDIO 0|1
```

| Command               | Effect                                                     |
| --------------------- | ---------------------------------------------------------- |
| `SPEED <s>`           | Change speed (same semantics as `PLAY`)                    |
| `PAUSE`               | Set speed to `0`                                           |
| `SEEK <n>`            | Absolute position (frame `n`)                              |
| `SEEK +<n>`           | `n` frames forward                                         |
| `SEEK -<n>`           | `n` frames backward                                        |
| `SEEK \|<n>`          | `n` frames before the current live end                     |
| `LENGTH <n>`          | Change maximum playback length (`0` = unlimited)           |
| `AUDIO 0` / `AUDIO 1` | Mute / unmute audio during playback                       |

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

## Known Limitations

- A sample-rate mismatch between recording and playback channel causes incorrect
  audio pitch. The sample rate is stored in the header (v3) and logged on open.
- Field-order heuristic in the consumer: HD interlaced (≥720 lines) → upper-field-first,
  SD interlaced → lower-field-first. Custom formats with a different field order
  may need to be added here.
- File format version 1 (CasparCG ≤ 2.0) is no longer supported.
- **Version 2 (before sample-rate extension)**: partially readable.
  - **Linear playback (`SPEED 1`) works** — the index stream is read sequentially.
  - **`SEEK`, `SPEED ≠ 1`, pause/resume, and reverse playback do not work**, because
    `INDEX_DATA_OFFSET` now assumes the v3 layout (16-byte extended header) and is
    off by 4 bytes for v2 files (12 bytes).
  - Sample rate falls back to 48000 Hz.
  - Recommendation: re-record with the current code once the file is needed for
    anything other than plain linear playback. A warning is logged on open.

## Source Files

- [replay.cpp](replay.cpp) – module registration, libjpeg version detection
- [consumer/replay_consumer.cpp](consumer/replay_consumer.cpp) – recording
- [producer/replay_producer.cpp](producer/replay_producer.cpp) – playback (incl. slow-motion blending)
- [util/file_operations.{h,cpp}](util/file_operations.h) – `.mav`/`.idx` header and JPEG I/O
- [util/frame_operations.{h,cpp}](util/frame_operations.h) – field/frame interlacing, blending

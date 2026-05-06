# MAV Replay File Format

## Overview

A replay recording consists of two paired files:

- **`.mav`** — Contains encoded frame data (Motion-JPEG video + audio)
- **`.idx`** — Contains file header metadata and the frame index table

Both files must be present to play back a recording. The index file is written alongside the data file and can be read while recording is still in progress (live-edge playback).

---

## 1. File Header (`.idx`, first 86 bytes)

All header structures use `#pragma pack(1)` for exact binary layout.

### `mjpeg_file_header` (66 bytes)

| Offset | Size | Type | Content |
|--------|------|------|---------|
| 0 | 4 | `char[4]` | Magic bytes: `'OMAV'` |
| 4 | 1 | `uint8_t` | Format version (current: `4`) |
| 5 | 4 | `uint32_t` | Frame width in pixels (full frame, including interlaced) |
| 9 | 4 | `uint32_t` | Frame height in pixels (full frame) |
| 13 | 8 | `double` | Frame rate — fields/s for interlaced, frames/s for progressive |
| 21 | 1 | `uint8_t` | Field mode: `1` = Lower-First, `2` = Upper-First, `3` = Progressive |
| 22 | 16 | `ptime` | `begin_timecode` — UTC absolute time at recording start |

### `mjpeg_file_header_ex` (20 bytes, immediately follows)

| Offset | Size | Content |
|--------|------|---------|
| 66 | 4 | Video FourCC: `'mjpg'` |
| 70 | 4 | Audio FourCC: `'in32'` |
| 74 | 4 | `int` — number of audio channels (e.g. 16) |
| 78 | 4 | `int` — audio sample rate in Hz (e.g. 48000) |

**Constant:** `INDEX_DATA_OFFSET = 86` — index entries begin at this byte offset.

---

## 2. Index Entries (starting at byte 86 in `.idx`)

### Version History

| Version | Entry Size | Hardware Timestamps | Status |
|---------|------------|---------------------|--------|
| v2 | 8 bytes | No | Deprecated — linear playback only |
| v3 | 8 bytes | No | Supported — no time-based seeking |
| **v4** | **16 bytes** | **Yes (microseconds)** | **Current** |

### `index_entry_v4` (16 bytes, version 4)

```
int64_t  file_offset             // Byte offset of the frame entry in the .mav file
int64_t  timestamp_microseconds  // Microseconds since begin_timecode
                                 // INT64_MIN = gap marker (no valid hardware timestamp)
```

**Address of frame N in the index file:**
```
index_byte_offset = 86 + N × 16
```

**Gap frames:** When `timestamp_microseconds == INT64_MIN`, no hardware timestamp was
available for that frame (signal dropout, non-Decklink source, etc.). The producer freezes
the last valid frame and sets `file/gap_detected = true` in its state output.

---

## 3. Frame Data in the `.mav` File

Each frame entry in the `.mav` file has the following layout:

```
[4 bytes]  uint32_t  audio_size      — Length of the audio block in bytes
[N bytes]  int32_t[] audio_samples   — Audio as signed 32-bit integers, little-endian
                                       (sample count = audio_size / 4)
[rest]     uint8_t[] jpeg_data       — JPEG image (Motion-JPEG, no container)
```

### Video

- Input color space: BGRA (CasparCG 2.5 native format)
- Stored as: RGB JPEG
- Encoder: libjpeg-turbo (CPU) or nvJPEG (GPU, optional)
- Quality: 1–100, default 90
- Chroma subsampling: Y444, Y422 (default), Y420, Y411

### Audio

- Format: signed 32-bit integers, little-endian
- Channels and sample rate are fixed at recording time and stored in the file header
- For interlaced recordings, each field entry carries its own audio block

### Interlaced recordings

Each `.mav` entry contains **one field** (half height). Two consecutive entries form one
complete interlaced frame. Field order follows the `field_mode` value in the header:
`1` = Lower-First, `2` = Upper-First.

---

## 4. Reading a Frame — Step by Step

```
1. Seek .idx to:  86 + N × 16
2. Read 16 bytes: { file_offset, timestamp_us }
3. Seek .mav to:  file_offset
4. Read 4 bytes:  audio_size
5. Skip:          audio_size bytes  (audio data)
6. Decode JPEG from current position
```

---

## 5. Seeking

### Frame-based seeking (v3 and v4)

```
SEEK 100      → jump to frame 100 from the beginning
SEEK |50      → jump to frame (live_edge − 50)
```

### Time-based seeking (v4 only)

```
SEEK |1s          → live_edge − 1,000,000 µs
SEEK |1500ms      → live_edge − 1,500,000 µs
SEEK_ABS <ms>     → UTC milliseconds (absolute wall-clock time)
```

`SEEK_ABS` uses **binary search** on `timestamp_microseconds` values in the index.
Complexity: O(log n). For 10 million frames: ~24 comparisons, under 1 ms.

Gap entries (`INT64_MIN`) are treated as if their timestamp is less than any target, so
binary search always lands on the next valid frame after a gap.

### Absolute time seek formula (for multi-channel synchronization)

```cpp
// Convert a UTC millisecond target to a relative microsecond offset:
auto epoch = boost::posix_time::ptime(boost::gregorian::date(1970, 1, 1));
int64_t begin_epoch_ms = (begin_timecode - epoch).total_milliseconds();
int64_t target_us = (target_epoch_ms - begin_epoch_ms) * 1000;
producer.seek_by_time(target_us);
```

---

## 6. Live-Edge Playback (open-ended files)

The producer supports reading a `.idx` file while the consumer is still writing it:

1. At startup, polls the `.idx` file size until it is greater than 0.
2. Calls `length_index_v4()` each decoder iteration to get the current frame count.
3. When the decoder reaches the live edge, it sleeps one frame period instead of busy-waiting.
4. No restart is required as new frames arrive — the producer adapts continuously.

---

## 7. AMCP State Output

The producer reports playback state via the `INFO` AMCP command:

| Key | Content |
|-----|---------|
| `file/live_edge_absolute_ms` | UTC milliseconds of the most recently recorded frame |
| `file/time_behind_live_ms` | Distance in ms between current playback position and live edge |
| `file/gap_detected` | `true` when the current frame is a gap entry |

---

## 8. Version Detection and Compatibility

```cpp
is_v4_ = (header.version >= 4);

// Version-aware dispatch:
long long len    = is_v4_ ? length_index_v4(idx) : length_index(idx);
int seek_result  = is_v4_ ? seek_index_v4(idx, frame, FILE_BEGIN)
                           : seek_index(idx, frame, FILE_BEGIN);
```

- **v3 files:** All seeking and speed modes work. Time-based `SEEK` commands are silently
  ignored; frame-based seeking continues to function.
- **v2 files:** Sequential (linear) playback only. Seeking, speed changes, and
  pause/resume do not work. A header-mismatch warning is logged. Re-record with the
  current version.

---

## 9. Key Source Files

| File | Purpose |
|------|---------|
| `src/modules/replay/util/file_operations.h` | Struct definitions, constants, `INDEX_DATA_OFFSET` |
| `src/modules/replay/util/file_operations.cpp` | Binary I/O: v3/v4 read / write / seek, `read_timestamp_at()` |
| `src/modules/replay/consumer/replay_consumer.cpp` | Recording: JPEG encoding, writing index entries |
| `src/modules/replay/producer/replay_producer.cpp` | Playback: seeking, `seek_by_time()`, decoder thread |
| `src/modules/replay/README.md` | AMCP command reference, known limitations |
| `docs/dev-tasks/03_timecode-and-frame-interpolation.md` | Design decisions, gap handling, multi-channel sync |

# Replay Module (MAV)

Sport-Replay-Modul für CasparCG 2.5. Nimmt einen Channel als Motion-JPEG-Stream
(`.mav` + `.idx`) auf und kann ihn mit variabler Geschwindigkeit, vor- und
rückwärts wiedergeben — ohne dass die Aufnahme dafür gestoppt werden muss
(Live-Replay).

Portiert aus dem ursprünglichen CasparCG-2.0/2.2-Replay-Modul von TU Lodz und
auf die CasparCG-2.5-Architektur (BGRA, neue `frame_consumer`/`frame_producer`-Signaturen)
angepasst.

## Dateiformat

Eine Aufnahme besteht aus zwei Dateien im `media/`-Ordner:

| Datei            | Inhalt                                              |
| ---------------- | --------------------------------------------------- |
| `<name>.mav`     | Sequenz aus `[audio_size:u32][audio:int32 LE][JPEG]` pro Eintrag |
| `<name>.idx`     | Header + 64-Bit-Offset jedes Eintrags in `.mav`     |

**Aktuelle Version:** 3 (Stand 2026-04-28)

Header-Layout (`mjpeg_file_header` + `mjpeg_file_header_ex`):

```
magick[4]            = 'OMAV'
version              = 3
width, height        = volle Frame-Größe (auch bei interlaced)
fps                  = Felder/s (interlaced) bzw. Frames/s (progressive)
field_mode           = 1 (lower-first), 2 (upper-first), 3 (progressive)
begin_timecode       = UTC-Zeit beim Start
video_fourcc[4]      = 'mjpg'
audio_fourcc[4]      = 'in32'
audio_channels       = z.B. 16
audio_sample_rate    = Hz, z.B. 48000   (NEU in v3)
```

Bei **interlaced** Aufnahmen enthält jeder MAV-Eintrag genau **ein Halbbild**
(JPEG mit halber Höhe, z.B. 1920×540 für 1080i50). Zwei aufeinanderfolgende
Einträge ergeben ein Vollbild.
Bei **progressive** ist jeder Eintrag ein Vollbild.

## AMCP-Befehle

### Aufnahme — Consumer

```text
ADD <channel> REPLAY <filename> [QUALITY <q>] [SUBSAMPLING 444|422|420|411]
REMOVE <channel> <consumer-index>
```

| Parameter      | Default | Beschreibung                                   |
| -------------- | ------- | ---------------------------------------------- |
| `<filename>`   | REPLAY  | Basisname (ohne `.mav`/`.idx`), unter `media/` |
| `QUALITY`      | 90      | JPEG-Qualität 1–100                            |
| `SUBSAMPLING`  | 422     | Chroma-Subsampling                             |

Felder-/Sample-Rate werden automatisch aus dem `video_format_desc` des Channels
übernommen. Bei interlaced Channels (z.B. 1080i50) speichert der Consumer beide
Halbbilder einzeln und behält die Audio-Information beider Felder.

**Beispiele**

```text
ADD 1 REPLAY TEST3                              # Standard
ADD 1 REPLAY MATCH SUBSAMPLING 444 QUALITY 95   # Hochqualitativ
REMOVE 1 150                                    # Index 150 = Replay-Consumer
```

### Wiedergabe — Producer

```text
PLAY <channel>-<layer> <filename> [SEEK [|]<n>] [SPEED <s>] [LENGTH <n>] [AUDIO 0|1]
```

| Parameter | Default          | Beschreibung                                                    |
| --------- | ---------------- | --------------------------------------------------------------- |
| `SEEK`    | `0` (= Anfang)   | Frame-Position. `\|<n>` = `<n>` Frames vor dem aktuellen Ende.  |
| `SPEED`   | `1.0`            | Geschwindigkeit. Negativ = rückwärts. `0` = pausiert.           |
| `LENGTH`  | `0` (= unendlich)| Maximale Anzahl wiederzugebender Frames                         |
| `AUDIO`   | auto             | `1` = Audio aktiv, `0` = Stumm. Default: aktiv, falls Datei Audiokanäle hat. |

**Beispiele**

```text
PLAY 1-1 TEST3                          # Audio automatisch an wenn vorhanden
PLAY 1-1 TEST3 SPEED 0.25               # Vierfach-Slow-Motion
PLAY 1-1 TEST3 SPEED -1                 # Rückwärts
PLAY 1-1 TEST3 SEEK |100                # Start 100 Frames vor dem Live-Ende
PLAY 1-1 TEST3 LENGTH 250 AUDIO 0       # 10 Sekunden bei 25fps, stumm
```

### Steuerung während der Wiedergabe — `CALL`

```text
CALL <channel>-<layer> SPEED <s>
CALL <channel>-<layer> PAUSE
CALL <channel>-<layer> SEEK [+|-||]<n>
CALL <channel>-<layer> LENGTH <n>
CALL <channel>-<layer> AUDIO 0|1
```

| Befehl              | Wirkung                                                 |
| ------------------- | ------------------------------------------------------- |
| `SPEED <s>`         | Geschwindigkeit umsetzen (gleiche Semantik wie `PLAY`)  |
| `PAUSE`             | Setzt die Geschwindigkeit auf `0`                       |
| `SEEK <n>`          | Absolute Position (Frame `n`)                           |
| `SEEK +<n>`         | `n` Frames vorwärts                                     |
| `SEEK -<n>`         | `n` Frames rückwärts                                    |
| `SEEK \|<n>`        | `n` Frames vor dem aktuellen Live-Ende                  |
| `LENGTH <n>`        | Maximale Wiedergabelänge ändern (`0` = unendlich)       |
| `AUDIO 0` / `AUDIO 1` | Audio während der Wiedergabe aus- / einschalten       |

## Live-Replay-Workflow

```text
# 1) Aufnahme starten (z.B. auf Channel 1)
ADD 1 REPLAY MATCH

# 2) Auf einem zweiten Channel oder Layer abspielen
PLAY 2-1 MATCH SPEED 0.5 SEEK |200

# 3) Während der Wiedergabe auf eine andere Stelle springen
CALL 2-1 SEEK |400

# 4) Aufnahme stoppen
REMOVE 1 150
```

Der Producer wartet aktiv darauf, dass die `.idx`-Datei wächst, sodass er
schon während der laufenden Aufnahme zurück in die Vergangenheit springen kann.

## Interlaced testen

Eine echte Interlaced-Quelle ist nicht nötig — es reicht, den **CasparCG-Channel
selbst** auf einen Interlaced-Modus zu konfigurieren. Der Channel-Mixer
deinterlaced/interlaced intern, sodass der Replay-Consumer `field::a`/`field::b`
sieht, egal ob die Quelle progressiv ist (NDI/OBS, FFmpeg-Datei, AMB Loop, …).

```xml
<channel>
    <video-mode>1080i5000</video-mode>
    <consumers>
        <screen />
        <system-audio />
    </consumers>
</channel>
```

Im Log des Replay-Consumers sollte dann erscheinen:

```
replay_consumer[…] Recording 1920x1080 interlaced (UFF) @ 50 fps, 16ch @ 48000 Hz
```

## Bekannte Einschränkungen

- Sample-Rate-Mismatch zwischen Aufnahme und Wiedergabe-Channel führt zu falscher
  Audio-Tonhöhe. Die Sample-Rate steht im Header (v3) und wird beim Öffnen geloggt.
- Field-Order-Heuristik des Consumers: HD interlaced (≥720 Zeilen) → upper-field-first,
  SD interlaced → lower-field-first. Custom-Formate mit abweichender Field-Order
  müssen hier ggf. ergänzt werden.
- Dateiformatversion 1 (CasparCG ≤ 2.0) wird nicht mehr gelesen.
- **Version 2 (vor Sample-Rate-Erweiterung)**: nur teilweise lesbar.
  - **Lineare Wiedergabe (`SPEED 1`) funktioniert** — der Index-Stream wird sequenziell gelesen.
  - **`SEEK`, `SPEED ≠ 1`, Pause/Resume und Rückwärtswiedergabe funktionieren nicht**, da `INDEX_DATA_OFFSET` jetzt das v3-Layout (16-Byte-Extended-Header) annimmt und bei v2-Dateien (12 Bytes) um 4 Bytes verrutscht.
  - Sample-Rate fällt auf 48000 Hz zurück.
  - Empfehlung: Aufnahme mit aktuellem Code wiederholen, sobald die Datei nicht mehr nur für reine Linear-Wiedergabe gebraucht wird. Beim Öffnen wird eine Warnung im Log ausgegeben.

## Quelldateien

- [replay.cpp](replay.cpp) – Modul-Registrierung, libjpeg-Versionsdetektion
- [consumer/replay_consumer.cpp](consumer/replay_consumer.cpp) – Aufnahme
- [producer/replay_producer.cpp](producer/replay_producer.cpp) – Wiedergabe (incl. Slow-Motion-Blending)
- [util/file_operations.{h,cpp}](util/file_operations.h) – `.mav`/`.idx`-Header und JPEG-I/O
- [util/frame_operations.{h,cpp}](util/frame_operations.h) – Field-/Frame-Interlacing, Blending

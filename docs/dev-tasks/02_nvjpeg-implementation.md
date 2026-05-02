# NVIDIA nvJPEG Hardware-Beschleunigung für den Replay-Consumer

## Status

| | |
|---|---|
| **Branch** | `feature/nvjpeg-implementation` |
| **Encoder (Consumer)** | ✅ Implementiert und auf Dev PC verifiziert (CUDA 13.2, RTX 3070) |
| **Decoder (Producer)** | ⬜ Noch nicht implementiert (niedrige Priorität) |
| **Decklink-Test** | 🔲 Ausstehend — interlaced Input noch nicht unter Echtbedingungen getestet |
| **Performance-Benchmark** | 🔲 Ausstehend — nach Decklink-Test |

---

## Ziel

Den JPEG-Codec im Replay-Modul optional durch NVIDIAs `nvJPEG`-Bibliothek ersetzen,
um auf einer RTX 3070 deutlich mehr Streams bei höherer Auflösung (1080p50) verarbeiten zu können.

- Aktuell (CPU): max. ~3× 1080i50 (libjpeg-turbo, Qualität 90, Y422)
- Erwartet (GPU): ~4–6× 1080p50 auf RTX 3070 (~6–10× Speedup pro Frame)

---

## Hardware / Software

| | Dev PC | Prod PC |
|---|---|---|
| GPU | RTX 3070 | RTX 3070 |
| NVIDIA Treiber | 591.74 | 591.74 (gleich) |
| CUDA Toolkit | 13.2 (installiert) | — (nur Treiber) |
| Decklink | — | Decklink Duo 2 |
| OS | Windows 11 Pro | Windows 11 Pro |

---

## Architektur

### Codec-Pfad nach Implementierung

```
Consumer:  Frame (BGRX)
               ↓
           jpeg_encoder::encode()   ← cpu_encoder_impl  ODER  nvjpeg_encoder_impl
               ↓ JPEG-Bytes (std::vector<uint8_t>)
           write_frame_encoded()    ← schreibt Audio-Block + JPEG in .mav
               ↓
           .mav-Datei

Producer:  .mav → file_operations.cpp:read_frame() → libjpeg-turbo → RGB → Frame
           (Producer-Seite unverändert, nvJPEG-Decoder noch nicht implementiert)
```

### Neue / geänderte Dateien

| Datei | Art | Beschreibung |
|---|---|---|
| `util/jpeg_codec.h` | neu | Abstrakte `jpeg_encoder`-Schnittstelle |
| `util/jpeg_codec_cpu.cpp` | neu | CPU-Encoder via libjpeg-turbo (`jpeg_mem_dest`) |
| `util/jpeg_codec_nvjpeg.cpp` | neu | GPU-Encoder via nvJPEG (nur bei `ENABLE_NVJPEG`) |
| `util/file_operations.h/.cpp` | geändert | `write_frame_encoded()` hinzugefügt |
| `consumer/replay_consumer.cpp` | geändert | `encoder_`-Member, neuer Encode-Pfad |
| `CMakeLists.txt` | geändert | `ENABLE_NVJPEG`-Option + POST_BUILD DLL-Copy |

### Encoder-Schnittstelle (`jpeg_codec.h`)

```cpp
class jpeg_encoder {
public:
    virtual bool encode(
        const uint8_t* bgrx_src,
        int width, int height,
        int src_stride,            // width*4 (progressiv) oder width*8 (interlaced Feld)
        std::vector<uint8_t>& out_jpeg) = 0;
};
```

Der `src_stride`-Parameter ermöglicht Interlaced-Felder ohne Zwischenkopie:
- Progressiv: `bgrx_src = Frame[0]`, `src_stride = width * 4`
- Interlaced UPPER: `bgrx_src = Frame[0]`, `src_stride = width * 4 * 2`
- Interlaced LOWER: `bgrx_src = Frame[1]`, `src_stride = width * 4 * 2`

### BGRX→BGR Konvertierung im nvJPEG-Encoder

Der BGRX→BGRI Stride-Trick (pitch=width*4) funktioniert **nicht** korrekt mit
`NVJPEG_INPUT_BGRI`, da nvJPEG intern 3 Byte pro Pixel liest — das X-Byte würde
in benachbarte Pixel eingemischt. Stattdessen wird BGRX **auf der CPU** zu BGR
konvertiert (Schleife in `nvjpeg_encoder_impl::encode`) und dann als kompaktes
BGR-Bild auf die GPU hochgeladen. Ein CUDA-Kernel könnte diese Konvertierung
bei Bedarf auf die GPU verlagern (Optimierung, noch nicht umgesetzt).

### Laufzeit-Fallback

```
ENABLE_NVJPEG=ON, nvjpegCreate() schlägt fehl
    → Warning im Log
    → Automatischer Fallback auf cpu_encoder_impl

ENABLE_NVJPEG=OFF (Default)
    → Immer cpu_encoder_impl
```

---

## Build

### Voraussetzungen

- CUDA Toolkit 13.x auf dem **Build-Rechner** installiert
- `nvcc` ist standardmäßig **nicht im PATH** — `CUDAToolkit_ROOT` explizit setzen

### CMake konfigurieren

```powershell
cmake -DENABLE_NVJPEG=ON `
      -DCUDAToolkit_ROOT="C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.2" `
      "C:\...\CasparCG-Server-MAV-Edition\build"
```

### Nur das Replay-Modul neu bauen

```powershell
cmake --build "...\build" --config Release --target replay
```

---

## Deployment auf den Prod-PC

Der Release-Ordner (`build/shell/Release/`) enthält nach dem Build automatisch
alle nötigen Dateien — CMake kopiert die CUDA-DLLs per POST_BUILD-Schritt:

```
casparcg.exe
cudart64_13.dll    ← CUDA Runtime (automatisch kopiert)
nvjpeg64_13.dll    ← nvJPEG (automatisch kopiert)
... (alle anderen DLLs wie bisher)
```

Den gesamten Release-Ordner zippen und auf den Prod-PC übertragen.
Auf dem Prod-PC muss **kein** CUDA Toolkit installiert sein — der NVIDIA-Treiber
(≥ 591.74) reicht aus.

### Verifikation im Log

Beim ersten `ADD ... REPLAY`-Befehl erscheinen:
```
[info]  nvJPEG encoder initialized on GPU: NVIDIA GeForce RTX 3070 (CUDA compute 8.6), quality=90
[info]  replay_consumer: JPEG encoder = nvJPEG (GPU)
```

Falls nvJPEG nicht verfügbar:
```
[warning] replay_consumer: nvJPEG unavailable, falling back to CPU encoder (libjpeg-turbo)
```

---

## Bekannte Risiken und offener Stand

| Risiko | Status | Notiz |
|---|---|---|
| BGRX→BGRI Stride-Trick funktioniert nicht | ✅ Gelöst | CPU-Konvertierung BGRX→BGR vor GPU-Upload |
| CUDA Toolkit nicht im PATH | ✅ Gelöst | `CUDAToolkit_ROOT` in CMake-Aufruf |
| CUDA-DLLs fehlen im Deployment | ✅ Gelöst | POST_BUILD kopiert DLLs automatisch |
| Interlaced Feld-Extraktion korrekt? | 🔲 Test ausstehend | Decklink-Test mit 1080i50 |
| Mehrere Consumer auf einer GPU: VRAM-Druck | 🔲 Offen | Nach Performance-Benchmark bewerten |
| nvJPEG Decoder (Producer-Seite) | ⬜ Nicht implementiert | Optional, niedrige Priorität |

---

## Nächste Schritte

1. **Decklink-Test** auf Prod PC: interlaced 1080i50-Input aufnehmen und Wiedergabe auf
   Artefakte prüfen (Zeilenversatz, Farbrauschen → wäre ein Fehler in der Feldextraktion)
2. **Performance-Benchmark**: Wie viele parallele Streams schafft die RTX 3070?
   Ziel: 4–6× 1080p50 (Vergleich: 3× 1080i50 auf CPU)
3. **Optional**: nvJPEG-Decoder für den Producer (Wiedergabe-Seite)

---

## Nicht in dieser Aufgabe enthalten

- Linux-Kompatibilität → separate Aufgabe `01_linux-compatibility.md`
- Codec-Wechsel zu H.264/HEVC (radikaler Eingriff, separates Projekt)

# Aufgabe: NVIDIA nvJPEG Hardware-Beschleunigung für den Replay-Consumer

## Kontext für den Assistenten

Du arbeitest am Projekt **CasparCG Server MAV Edition** — eine auf CasparCG 2.6 portierte
Variante mit einem eigenen Replay-Consumer/Producer für Broadcast-Anwendungen.

**Ziel**: Den JPEG-Codec im Replay-Modul optional durch NVIDIAs `nvJPEG`-Bibliothek
ersetzen, um auf einer RTX 3070 deutlich mehr Streams bei höherer Auflösung (1080p50)
verarbeiten zu können.

**Motivation**:
- Aktuell: max. ~3× 1080i50 auf CPU (libjpeg-turbo)
- Mit nvJPEG auf RTX 3070: ~4–6× 1080p50 erwartet (~6-10× Speedup pro Frame)

---

## Voraussetzungen (vom Benutzer vor dem Start prüfen)

1. **CUDA Toolkit** installiert? Mindestens Version 10.0 (nvJPEG verfügbar ab 10.0).
   Empfohlen: CUDA 12.x (aktuell, volle RTX 30xx Unterstützung).
   Prüfen: `nvcc --version` im Terminal.

2. **NVIDIA Treiber** aktuell? Für CUDA 12.x: Treiber ≥ 525.
   Prüfen: `nvidia-smi`.

3. **Aktueller Build funktioniert** auf Windows mit libjpeg-turbo? (Baseline)

---

## Architektur des Replay-Moduls (Ist-Stand)

### Codec-Pfad aktuell (nur libjpeg-turbo)

```
Consumer:  Frame (BGRX) → file_operations.cpp:write_frame() → libjpeg-turbo → JPEG → .mav
Producer:  .mav → file_operations.cpp:read_frame()  → libjpeg-turbo → RGB  → Frame
```

### Relevante Dateien

| Datei | Relevante Zeilen | Funktion |
|---|---|---|
| `src/modules/replay/util/file_operations.cpp` | 416–568 | JPEG encode (`write_frame`) + decode (`read_frame`) |
| `src/modules/replay/util/file_operations.h` | 100–130 | `chroma_subsampling` Enum, Funktionsdeklarationen |
| `src/modules/replay/consumer/replay_consumer.cpp` | 37–39 | Qualitäts- und Subsampling-Konstanten |
| `src/modules/replay/CMakeLists.txt` | — | Build-Konfiguration (hier nvJPEG hinzufügen) |

### Encoding-Kontext (Zeilen 486–568 in file_operations.cpp)

Eingabe: `uint8_t*` BGRX-Puffer, `width`, `height`, `quality`, `chroma_subsampling`
Ausgabe: JPEG-Bytes in custom memory buffer via libjpeg destination manager

### Decoding-Kontext (Zeilen 416–482 in file_operations.cpp)

Eingabe: JPEG-Bytes aus `.mav`-Datei
Ausgabe: `uint8_t*` RGB-Puffer (3 Byte/Pixel)
Konvertierung danach in `replay_producer.cpp:271–278`: RGB → BGRA

---

## Implementierungsplan

### Schritt 1: Abstrakte Codec-Schnittstelle einführen

Neue Datei: `src/modules/replay/util/jpeg_codec.h`

```cpp
#pragma once
#include <cstdint>
#include <vector>

namespace caspar { namespace replay {

// Abstrakte Schnittstelle für JPEG-Encoding
class jpeg_encoder {
public:
    virtual ~jpeg_encoder() = default;
    // Enkodiert BGRX-Frame zu JPEG. Gibt Anzahl geschriebener Bytes zurück.
    virtual size_t encode(
        const uint8_t* bgrx_src, int width, int height,
        uint8_t* jpeg_dst, size_t dst_capacity) = 0;
};

// Abstrakte Schnittstelle für JPEG-Decoding
class jpeg_decoder {
public:
    virtual ~jpeg_decoder() = default;
    // Dekodiert JPEG zu RGB (3 Byte/Pixel). Gibt false bei Fehler.
    virtual bool decode(
        const uint8_t* jpeg_src, size_t src_size,
        uint8_t* rgb_dst, int width, int height) = 0;
};

// Factory-Funktionen (implementiert in jpeg_codec_cpu.cpp und jpeg_codec_nvjpeg.cpp)
std::unique_ptr<jpeg_encoder> create_cpu_encoder(int quality, int subsampling);
std::unique_ptr<jpeg_decoder> create_cpu_decoder();

#ifdef ENABLE_NVJPEG
std::unique_ptr<jpeg_encoder> create_nvjpeg_encoder(int quality, int subsampling);
std::unique_ptr<jpeg_decoder> create_nvjpeg_decoder();
#endif

}} // namespace
```

### Schritt 2: CPU-Implementierung auslagern

Den bestehenden libjpeg-turbo-Code aus `file_operations.cpp` (Zeilen 416–568) in
eine neue Datei `jpeg_codec_cpu.cpp` extrahieren und hinter die `jpeg_encoder`/
`jpeg_decoder` Schnittstelle legen.

### Schritt 3: nvJPEG-Implementierung

Neue Datei: `src/modules/replay/util/jpeg_codec_nvjpeg.cpp`

```cpp
#ifdef ENABLE_NVJPEG
#include "jpeg_codec.h"
#include <nvjpeg.h>
#include <cuda_runtime.h>

namespace caspar { namespace replay {

class nvjpeg_encoder_impl : public jpeg_encoder {
    nvjpegHandle_t         handle_;
    nvjpegEncoderState_t   state_;
    nvjpegEncoderParams_t  params_;
    cudaStream_t           stream_;
    // GPU-Puffer für Input-Frame
    uint8_t*               d_bgrx_ = nullptr;
    size_t                 d_bgrx_size_ = 0;

public:
    nvjpeg_encoder_impl(int quality, int subsampling) {
        nvjpegCreate(NVJPEG_BACKEND_DEFAULT, nullptr, &handle_);
        cudaStreamCreate(&stream_);
        nvjpegEncoderStateCreate(handle_, &state_, stream_);
        nvjpegEncoderParamsCreate(handle_, &params_, stream_);
        nvjpegEncoderParamsSetQuality(params_, quality, stream_);
        // Subsampling mapping: Y422 → NVJPEG_CSS_422, Y420 → NVJPEG_CSS_420
        nvjpegEncoderParamsSetSamplingFactors(params_,
            subsampling == 0 ? NVJPEG_CSS_444 :
            subsampling == 1 ? NVJPEG_CSS_422 :
                               NVJPEG_CSS_420, stream_);
    }

    ~nvjpeg_encoder_impl() {
        if (d_bgrx_) cudaFree(d_bgrx_);
        nvjpegEncoderParamsDestroy(params_);
        nvjpegEncoderStateDestroy(state_);
        cudaStreamDestroy(stream_);
        nvjpegDestroy(handle_);
    }

    size_t encode(const uint8_t* bgrx_src, int width, int height,
                  uint8_t* jpeg_dst, size_t dst_capacity) override
    {
        // 1. GPU-Puffer allozieren/wiederverwenden
        size_t needed = (size_t)width * height * 4;
        if (d_bgrx_size_ < needed) {
            if (d_bgrx_) cudaFree(d_bgrx_);
            cudaMalloc(&d_bgrx_, needed);
            d_bgrx_size_ = needed;
        }
        // 2. BGRX auf GPU hochladen
        cudaMemcpyAsync(d_bgrx_, bgrx_src, needed,
                        cudaMemcpyHostToDevice, stream_);
        // 3. nvjpegImage einrichten (interleaved BGRI — X-Kanal ignoriert)
        nvjpegImage_t nv_img{};
        nv_img.channel[0] = d_bgrx_;
        nv_img.pitch[0]   = width * 4;  // stride inkl. X-Byte
        // HINWEIS: NVJPEG_INPUT_BGRI liest 3 Byte pro Pixel ab channel[0].
        // Mit pitch=width*4 überspringt nvJPEG korrekt das X-Byte.
        // Dies muss beim ersten Build verifiziert werden — falls Bildfehler
        // auftreten, BGRX→BGR Konvertierung via cuBGRX2BGR Kernel nötig.
        nvjpegEncodeImage(handle_, state_, params_, &nv_img,
                          NVJPEG_INPUT_BGRI, width, height, stream_);
        // 4. Bitstream holen
        size_t length = 0;
        nvjpegEncodeRetrieveBitstream(handle_, state_, nullptr, &length, stream_);
        cudaStreamSynchronize(stream_);
        if (length > dst_capacity) return 0; // Puffer zu klein
        nvjpegEncodeRetrieveBitstream(handle_, state_, jpeg_dst, &length, stream_);
        cudaStreamSynchronize(stream_);
        return length;
    }
};

std::unique_ptr<jpeg_encoder> create_nvjpeg_encoder(int quality, int subsampling) {
    return std::make_unique<nvjpeg_encoder_impl>(quality, subsampling);
}

// Analog: nvjpeg_decoder_impl mit nvjpegDecode() ...

}} // namespace
#endif // ENABLE_NVJPEG
```

**Wichtiger Hinweis zur Farbraumkonvertierung**: Das BGRX→BGRI-Stride-Trick
(pitch = width*4 bei NVJPEG_INPUT_BGRI) muss beim ersten Build auf Korrektheit
geprüft werden. nvJPEG liest intern 3 Byte und springt dann zum nächsten Pixel
anhand des Pitch — das Verhalten bei 4-Byte-Pitch ist API-abhängig und muss
empirisch verifiziert werden. Falls das Bild Artefakte zeigt, muss ein
CUDA-Kernel für BGRX→BGR Konvertierung hinzugefügt werden.

### Schritt 4: CMake-Änderungen

In `src/modules/replay/CMakeLists.txt` ergänzen:

```cmake
# ── optionales nvJPEG ─────────────────────────────────────────────────────────
option(ENABLE_NVJPEG "GPU-beschleunigtes JPEG via NVIDIA nvJPEG" OFF)

if(ENABLE_NVJPEG)
    find_package(CUDAToolkit REQUIRED)
    target_compile_definitions(replay PRIVATE ENABLE_NVJPEG)
    target_link_libraries(replay PRIVATE
        CUDA::cudart
        CUDA::nvjpeg
    )
    # nvJPEG-Quelldatei nur wenn aktiviert
    target_sources(replay PRIVATE util/jpeg_codec_nvjpeg.cpp)
endif()
```

Build-Aufruf mit nvJPEG:
```bash
cmake -DENABLE_NVJPEG=ON -DCMAKE_BUILD_TYPE=Release ..
```

### Schritt 5: Laufzeit-Selektion in file_operations.cpp

```cpp
// Globale Encoder-Instanz (eine pro Prozess oder pro Consumer-Instanz)
// In replay_consumer.cpp, Konstruktor:
#ifdef ENABLE_NVJPEG
    encoder_ = create_nvjpeg_encoder(quality_, subsampling_);
#else
    encoder_ = create_cpu_encoder(quality_, subsampling_);
#endif
```

**Thread-Sicherheit**: Jede Consumer-Instanz bekommt ihre eigene
`nvjpegHandle_t`-Instanz. Die nvJPEG-States sind **nicht thread-safe**,
dürfen also nicht zwischen Instanzen geteilt werden.

---

## Bekannte Risiken und offene Fragen

| Risiko | Wahrscheinlichkeit | Lösung |
|---|---|---|
| BGRX→BGRI Stride-Trick funktioniert nicht | mittel | CUDA-Kernel für BGRX→BGR schreiben |
| CUDA Toolkit nicht installiert | gering (RTX 3070 vorhanden) | Toolkit installieren, Pfad in CMAKE_PREFIX_PATH |
| nvJPEG Encoder-Output weicht von libjpeg ab | gering | Dekodierpfad (libjpeg) bleibt kompatibel |
| Mehrere Consumer auf einer GPU: VRAM-Druck | mittel bei >4 Streams | VRAM-Bedarf überwachen, ggf. GPU-Puffer teilen |
| nvJPEG Decoder auf Windows: CUVID-Konflikte | unbekannt | Test erforderlich |

---

## Reihenfolge der Implementierung

1. Zuerst nur **Encoder** (Consumer) mit nvJPEG implementieren — größter Gewinn
2. Dann **Decoder** (Producer) — optional, da Wiedergabe weniger kritisch
3. Laufzeit-Fallback auf CPU wenn `nvjpegCreate()` fehlschlägt (kein CUDA-Gerät)

---

## Nicht in dieser Aufgabe enthalten

- Linux-Kompatibilität → separate Aufgabe `01_linux-compatibility.md`
- Codec-Wechsel zu H.264/HEVC (radikaler Eingriff, separates Projekt)
- Performance-Benchmarks (werden nach der Implementierung durchgeführt)

---

## Hardware/Software des Entwicklers

- HP Z1, **RTX 3070**, Decklink-Karte
- CUDA Toolkit: vor Implementierung installieren (Version prüfen mit `nvcc --version`)
- OS: Windows 11 Pro (primär), Linux (sekundär)
- Aktuell: max. 3× 1080i50 auf CPU (libjpeg-turbo, Qualität 90, Y422)
- Ziel: 4–6× 1080p50 mit GPU

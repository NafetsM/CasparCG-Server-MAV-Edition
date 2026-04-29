# Aufgabe: Linux-Kompatibilität des Replay-Consumers herstellen

## Kontext für den Assistenten

Du arbeitest am Projekt **CasparCG Server MAV Edition** — eine auf CasparCG 2.6 portierte
Variante mit einem eigenen Replay-Consumer/Producer für Broadcast-Anwendungen.

Das Projekt liegt unter `src/modules/replay/`. Es gibt bereits ein `Bootstrap_Linux.cmake`
für das Gesamtprojekt, aber der Replay-Consumer wurde bisher nur unter Windows gebaut und
getestet. Ziel dieser Aufgabe ist es, den Replay-Consumer vollständig Linux-kompatibel
zu machen.

---

## Architektur des Replay-Moduls (Zusammenfassung der Vorab-Analyse)

| Datei | Funktion |
|---|---|
| `src/modules/replay/util/file_operations.h/.cpp` | JPEG-Kodierung/-Dekodierung (libjpeg-turbo), Datei-I/O |
| `src/modules/replay/util/frame_operations.h/.cpp` | Frame-Operationen (Interlacing, Blending) via Intel TBB |
| `src/modules/replay/consumer/replay_consumer.h/.cpp` | Aufnahme-Consumer, asynchrone JPEG-Encoding-Queue |
| `src/modules/replay/producer/replay_producer.h/.cpp` | Wiedergabe-Producer, Dekodierung + Seeking |
| `src/modules/replay/CMakeLists.txt` | Build-Konfiguration des Moduls |

**Codec**: libjpeg-turbo (direkt, kein FFmpeg im Replay-Modul)
**Dateiformat**: `.mav` (MJPEG + Audio) + `.idx` (Frame-Index)

---

## Was bereits portierbar ist (kein Änderungsbedarf)

In `src/modules/replay/util/file_operations.h` ist der I/O-Layer bereits korrekt
konditioniert:

```cpp
// Zeile 15-20
#ifdef _WIN32
#   define NOMINMAX
#   include <Windows.h>
#   define REPLAY_IO_WINAPI   // ← aktiviert Win32-Pfad
#endif
```

Auf Linux ist `_WIN32` nicht definiert → `REPLAY_IO_WINAPI` wird nicht gesetzt →
der POSIX-Fallback mit `FILE*`, `fopen64`, `fseek64` wird automatisch aktiv.

**Kein CMake-Patch nötig** für den I/O-Layer.

---

## Zu prüfende und zu behebende Punkte

### 1. `fopen64` auf Linux/x86_64

In `file_operations.h` (Zeilen 22-61) wird `fopen64` nur für `_WIN32` definiert,
nicht für Linux. Auf Linux x86_64 existiert `fopen64` als GNU-Extension (`_LARGEFILE64_SOURCE`).

**Prüfen**: Kompiliert `file_operations.cpp` auf Linux ohne explizites `fopen64`-Define?
Falls nicht, ergänzen:
```cpp
// in file_operations.h, nach dem __x86_64__ Block:
#elif defined(__linux__)
#   ifndef fopen64
#       define fopen64 fopen  // 64-bit fopen ist Standard auf 64-bit Linux
#   endif
```

### 2. `boost::posix_time::ptime` in packed struct

In `file_operations.h` (Zeile 83) ist `ptime` direkt in `mjpeg_file_header`
eingebettet — einem `#pragma pack(1)` Struct, das binär auf Disk geschrieben wird.

```cpp
struct mjpeg_file_header {
    ...
    boost::posix_time::ptime begin_timecode;  // ← binäre Repräsentation!
};
```

`#pragma pack(push,1)` ist auf GCC/Clang kompatibel. **Aber**: Die binäre
Größe von `ptime` könnte zwischen Compilern/Plattformen variieren.

**Zu prüfen**: `sizeof(mjpeg_file_header)` auf Linux == Windows?
Wenn nicht → Dateien sind nicht cross-platform lesbar (separates Problem, für
v2 zurückstellen; erst dokumentieren).

### 3. `wstring`-Verwendung für Dateinamen

Der Consumer verwendet `std::wstring` für Pfade (CasparCG-Standard). Auf Linux
ist `wchar_t` 4 Byte (UTF-32) statt 2 Byte (Windows UTF-16).

Der POSIX-Fallback nutzt bereits `u8()` Konvertierung aus `common/utf.h`:
```cpp
// file_operations.cpp (POSIX-Pfad):
fopen64(u8(filename).c_str(), fmode);
```
Das ist korrekt — hier gibt es keinen Handlungsbedarf, solange `common/utf.h`
auf Linux verfügbar ist.

**Prüfen**: Existiert und kompiliert `common/utf.h` auf Linux?

### 4. Windows-spezifische Includes in Consumer/Producer

**Zu prüfen**: Gibt es in diesen Dateien direkte Windows-Includes außerhalb von
`file_operations.h`?
- `src/modules/replay/consumer/replay_consumer.cpp`
- `src/modules/replay/producer/replay_producer.cpp`

Suche nach: `#include <Windows.h>`, `HANDLE`, `DWORD`, `CreateFile` in diesen Dateien.

### 5. CasparCG Core auf Linux

Das eigentliche Fragezeichen: Kompiliert der Rest von CasparCG (core, common,
ffmpeg-Modul) auf Linux? Das liegt außerhalb des Replay-Moduls.
`Bootstrap_Linux.cmake` existiert, was darauf hindeutet, dass es zumindest
vorgesehen war.

---

## Vorgeschlagene Vorgehensweise

1. **Neuen Branch erstellen**: `feature/linux-compat` von `port/caspar-25`
2. **Test-Build auf Linux** (Ubuntu 22.04 LTS empfohlen):
   ```bash
   mkdir build && cd build
   cmake -DCMAKE_BUILD_TYPE=Release ..
   make replay 2>&1 | tee build_replay.log
   ```
3. **Fehler aus `build_replay.log` auswerten** und gezielt beheben
4. Die oben genannten Punkte 1-4 abarbeiten

---

## Erwartete Änderungen (minimal)

- `src/modules/replay/util/file_operations.h`: ggf. `fopen64` für Linux ergänzen
- `src/modules/replay/CMakeLists.txt`: ggf. Linux-spezifische Compile-Flags
- **Keine** Änderungen an der `REPLAY_IO_WINAPI` Logik nötig

---

## Nicht in dieser Aufgabe enthalten

- NVIDIA GPU-Beschleunigung → separate Aufgabe `02_nvjpeg-implementation.md`
- Cross-platform Binärkompatibilität der `.mav`-Dateien (v2-Thema)
- Performance-Optimierungen

---

## Hardware/Software des Entwicklers

- HP Z1, RTX 3070, Decklink-Karte
- Aktuell: Windows-Only Build
- Ziel: auch Linux (Ubuntu) unterstützen
- Bisherige Performance: max. 3× 1080i50 auf CPU (libjpeg-turbo)

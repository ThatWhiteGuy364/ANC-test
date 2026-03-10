# WhiteLabs ANC — Active Noise Cancellation Engine
> Drafted using JuneAI, a creation of WhiteLabs, owned and ran by ThatWhiteGuy364

A real-time, software-only Active Noise Cancellation (ANC) Android application built with
Kotlin + C++ (Oboe), featuring a Stereo LMS adaptive filter, Radix-2 FFT visualizer,
and a Material You UI.

---

## Project Structure

```
WhiteLabsANC/
├── app/
│   ├── build.gradle.kts
│   └── src/main/
│       ├── AndroidManifest.xml
│       ├── cpp/
│       │   ├── CMakeLists.txt
│       │   └── LmsEngine.cpp          ← Core C++ ANC engine
│       ├── java/com/whitelabs/anc/
│       │   ├── MainActivity.kt        ← UI controller + JNI bridge
│       │   ├── AncService.kt          ← Foreground service
│       │   └── VisualizerView.kt      ← FFT canvas view
│       └── res/
│           ├── layout/activity_main.xml
│           └── values/
│               ├── themes.xml
│               └── colors.xml
├── build.gradle.kts
└── settings.gradle.kts
```

---

## Hardware Requirements

| Requirement | Detail |
|---|---|
| **External Microphone** | Place near the ear as "Reference Mic". Physical separation from speaker is critical. |
| **High-Speed Processing** | The anti-noise must meet the incoming wave at the exact moment of impact. |
| **Wired/BT Earphones** | Earphone mic acts as "error mic"; speaker outputs anti-noise. |
| **Android 8.0+ (API 26)** | Required for Oboe low-latency audio. |

---

## Calibration Steps

1. **Plug in wired earphones** (or connect Bluetooth headset). The app will prefer the headset mic automatically via `AudioManager.isBluetoothScoOn`.

2. **Mu (Step Size) Tuning** — Default `μ = 0.01`. If the system is unstable or screeches, reduce mu in `LmsEngine.cpp`:
   ```cpp
   static constexpr float kMu = 0.005f;     // Lower = more stable, slower adapt
   static constexpr float kMuHigh = 0.001f; // For high-frequency bands
   ```

3. **Filter Size** — Default `kFilterSize = 128`. Larger = better low-frequency cancellation, more CPU.
   - Low frequencies: use `N = 256`
   - High frequencies: use `N = 32`

4. **Latency** — Oboe is configured for `PerformanceMode::LowLatency` + `SharingMode::Exclusive`. If you observe an echo instead of silence, the round-trip latency is too high.

5. **Feedback Loop Warning** — Because the engine "captures all audio," ensure thy logic does NOT pick up its own anti-noise output. Keep the Reference Mic physically separated from the speaker.

6. **Stereo Sink** — The C++ engine is configured for `ChannelCount::Stereo`, giving two independent cancellation streams.

---

## Architecture

```
Earphone Mic (Reference)
        │
        ▼
  [Oboe Input Stream]  ──────────────────────────────────────────┐
        │                                                          │
  ┌─────▼──────┐    error(n) = noise - antiNoise                  │
  │ LMS Filter │◄──────────────────────────────────────────────── │
  │ (per ch.)  │    w(n+1) = w(n) + μ·e(n)·x(n)                  │
  └─────┬──────┘                                                   │
        │ antiNoise                                                 │
        ▼                                                           │
  [Inverted Signal] ─► Oboe Output ─► Speaker                     │
        │                                      │                   │
        │                                 User hears               │
        │                                 silence ✓                │
        │                                                          │
        └──[FFT]──► VisualizerView (60fps canvas update) ◄────────┘
```

---

## Build Instructions

1. Open **Android Studio** (Hedgehog or newer).
2. `File → Open` → select the `WhiteLabsANC` directory.
3. Let Gradle sync. Oboe will download automatically via Maven.
4. Plug in a device running **Android 8.0+**.
5. Run `▶ Run 'app'`.

> **Note:** CMake 3.22.1 is required. Android Studio typically manages this via the SDK Manager (`SDK Tools → CMake`).

---

## Dependencies

| Library | Version | Purpose |
|---|---|---|
| Oboe | 1.8.0 | Low-latency audio I/O |
| Material3 | 1.11.0 | Material You UI |
| AppCompat | 1.6.1 | AndroidX base |
| Core-KTX | 1.12.0 | Kotlin extensions |

---

*WhiteLabs — owned and operated by ThatWhiteGuy364*

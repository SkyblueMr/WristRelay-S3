# Original firmware core

This ESP-IDF component contains selected first-party C source taken from the
original ESP32-S3 watch firmware. The module names and algorithms are retained;
this is not the complete original application or a production firmware image.
The exported source is covered by the repository license.

| Source | Responsibility |
| --- | --- |
| `board_audio_resampler.c`, `include/board_audio_resampler.h` | Streaming Q15 FIR for 24-to-16 kHz and 16-to-24 kHz, including the synchronized two-microphone plus playback-reference path. |
| `include/board_audio_output_fence.h` | Waits for four distinct DMA completion buffers in one playback epoch and tracks nonzero completions. The caller owns locking, internal DRAM placement, and matching the configured descriptor count. |
| `include/board_audio_timeline_core.h` | Fixed-capacity diagnostics for numeric playback events and a comparison hash, without storing samples. The caller owns the event buffer and serializes writes. |
| `include/board_audio_afe_policy.h`, `include/board_mn_signal.h` | Two-mic/reference frame order, exact fetch accounting, and bounded signal statistics used around speech recognition. They do not perform echo cancellation or wake-word inference. |
| `include/board_voice_uplink_pacing.h`, `include/board_voice_failure_policy.h` | Source-media-clock scheduler policy for a 20 ms packet backlog and numeric attribution for local audio failures. They do not implement a relay or a wire protocol. |
| `include/board_ota_retry.h` | Capped retry schedule for online attempts. It does not download, validate, sign, or install updates. |

The Opus uplink and the optional WakeNet adapter call the original resampler
directly for 24-to-16 kHz conversion. The other policies are opt-in headers
with no runtime side effects until called. Their presence does not enable the original
application's wake, AFE, reconnect, DMA drain, or OTA workflows in the public
app. In another ESP-IDF project, add this directory to `EXTRA_COMPONENT_DIRS`
and list `firmware_core` in the consuming component's `REQUIRES`.

No device configuration, model assets, keys, private endpoints, recordings,
proprietary SDK overlays, or production transport contracts are needed by these
modules. The app and hardware setup are documented in the repository README.

## Host validation

From the repository root in Linux or WSL:

```sh
cmake -S firmware_core -B /tmp/wristrelay-core -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/wristrelay-core
ctest --test-dir /tmp/wristrelay-core --output-on-failure
```

The resampler regression adapts the original synthetic-sample tests to the
Q15 implementation and checks the fused three-channel path. The policy tests
use synthetic timestamps, DMA identities,
and PCM values. The test-only `esp_attr.h` deliberately does not emulate IRAM
placement, interrupt timing, DMA, signal quality, or device behavior.

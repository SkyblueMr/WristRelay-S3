# WristRelay-S3

WristRelay-S3 turns a **Waveshare ESP32-S3-Touch-AMOLED-2.06** into a two-way voice terminal for a WebSocket gateway you run. The watch handles microphone capture, Opus, speaker playback, a small call-status display and, optionally, local wake-word detection. The gateway decides what to do with the audio; no cloud account or AI service is built in.

This is a standalone public port, not an update for an existing watch installation. It retains selected [original firmware modules](firmware_core/README.md), including the fixed-point resampler used on the microphone path. It does not include the original backend, provisioning service, OTA client, recordings or private wake-word model.

## Using the watch

| Action | Result |
| --- | --- |
| Tap **BOOT** after startup | Toggles the screen without ending the call. |
| Hold **BOOT** for about 1.2 seconds | Starts a call, or stops the current one. |
| Say **Hi, ESP** with the optional WakeNet profile | Starts a call when the watch and gateway are ready, without turning the screen on. The default build is button-only. |

The device does not continuously stream idle microphone audio. During a call it sends **one 60 ms mono Opus packet per binary WebSocket message**; the speaker receives the same framing in reverse. The uplink encodes at 16 kHz and the playback decoder runs at 24 kHz. See the [wire protocol](docs/protocol.md) before implementing a gateway. A generic AI or transcription API is not a drop-in replacement.

## Build

Use an activated **ESP-IDF 6.1.x** environment with the ESP32-S3 toolchain. The build downloads the pinned ESP-IDF components and a checksum-verified Opus source archive. No device or credentials are needed to compile.

```sh
git clone https://github.com/SkyblueMr/WristRelay-S3.git
cd WristRelay-S3
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

To make a call, open **WristRelay-S3** in `menuconfig` and set the Wi-Fi SSID and passphrase, the `wss://` URL of your gateway, and its bearer token if required. The watch needs a synchronized clock and a trusted server certificate. Network and gateway credentials are empty by default. Values entered here are embedded in the local `sdkconfig` and the resulting firmware: **do not publish either once configured**.

Use a dedicated development board with the [documented wiring](docs/board.md), 32 MB flash and octal PSRAM. The 2.16-inch board is not pin-compatible. This project supplies its own factory partition table; flashing it can replace an existing installation. Back up the board first. On a matching development board, use `idf.py -p PORT flash monitor`, replacing `PORT` with its serial port. Do not hold BOOT during a normal reset.

For the **optional wake-word build**, follow [Wake and idle power](docs/wake.md). It uses a separate build configuration and model partition. The model is fetched from Espressif; no model weights are stored in this repository.

## Testing and limits

The [Linux/WSL host tests](tests/README.md) exercise the resampler and timing policies, WebSocket frame assembly, button gestures, Wi-Fi power decisions and the Opus round trip. They use synthetic inputs, not recordings. They cannot establish acoustic accuracy, battery life or end-to-end latency. This port does not implement acoustic echo cancellation or automatic call reconnection. Test the full microphone-to-gateway-to-speaker path on your board before relying on it.

## License

Original project code is available under the [MIT License](LICENSE). The adapted panel initialization table and third-party dependencies retain their respective licenses; see [third-party notices](THIRD_PARTY_NOTICES.md).

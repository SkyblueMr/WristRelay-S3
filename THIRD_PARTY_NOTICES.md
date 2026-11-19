# Third-party notices

The repository's MIT license applies to the original WristRelay-S3 source and documentation, not to third-party dependencies. Dependencies obtained separately are governed by the notices shipped with their respective versions.

## Included third-party material

- The CO5300 panel initialization table in [waveshare_206_init.h](main/display/waveshare_206_init.h) is adapted from the [Waveshare ESP32-S3-Touch-AMOLED-2.06 BSP v2.0.0](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_2_06/versions/2.0.0/readme). The header remains under [Apache License 2.0](LICENSES/Apache-2.0.txt); the rest of the BSP is not included.

## Build and development dependencies

- [ESP-IDF](https://github.com/espressif/esp-idf/blob/master/LICENSE): Apache License 2.0 at the project level. Components bundled with ESP-IDF may carry their own notices.
- [Espressif WebSocket Client](https://github.com/espressif/esp-protocols/blob/master/components/esp_websocket_client/LICENSE): Apache License 2.0. The component is obtained separately, not vendored here.
- [Espressif Codec Device](https://github.com/espressif/esp-adf/blob/master/components/esp_codec_dev/LICENSE): Apache License 2.0. The component provides hardware codec drivers; it is obtained separately, not vendored here.
- [Espressif CO5300 LCD Driver](https://components.espressif.com/components/espressif/esp_lcd_co5300/versions/2.2.0/license): Apache License 2.0. The component is obtained separately, not vendored here.
- [Xiph.Org Opus 1.5.2](https://github.com/xiph/opus/blob/v1.5.2/COPYING): BSD-style terms and the upstream patent-license references are in Opus's COPYING file. The firmware build fetches and verifies an upstream source archive, not a prebuilt codec. No copy of the archive or library is distributed in this source tree.
- [Espressif ESP-SR 2.6.1](https://components.espressif.com/components/espressif/esp-sr/versions/2.6.1/license): the optional WakeNet profile fetches this component separately. Its license has an Espressif-products condition and is not the MIT license used for WristRelay-S3 source. No ESP-SR libraries or wake-word models are distributed in this source tree.
- [Python](https://docs.python.org/3/license.html): the release audit tool runs on a separately installed Python interpreter. Python and its standard library are not redistributed in this repository.

## Not included

- [Espressif ESP Audio Codec](https://components.espressif.com/components/espressif/esp_audio_codec) is distinct from both Xiph.Org Opus and Espressif Codec Device. The Espressif codec archive is not shipped here; its Espressif Modified MIT terms are not the upstream Opus BSD-style terms.
- No private fonts, graphics, audio recordings, model weights, firmware binaries, board-support package snapshots, or deployment materials are included.

If you redistribute a built firmware image, collect the applicable license texts and notices from the exact dependency versions in that build. This source-only notice is not a substitute for those materials.

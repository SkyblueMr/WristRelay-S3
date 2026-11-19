# Linux host tests

From the repository root, use Linux or WSL with a C compiler and CMake:

```sh
cmake -S tests -B /tmp/wristrelay-host-tests -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/wristrelay-host-tests --parallel
ctest --test-dir /tmp/wristrelay-host-tests --output-on-failure
```

The default build fetches the same hash-pinned Opus source used by the firmware.
Pass `-DWR_OPUS_ARCHIVE=/path/to/opus-1.5.2.tar.gz` to use a local copy, or
`-DWR_TEST_OPUS=OFF` to run the other tests without downloading Opus.

The tests exercise the exported WebSocket frame parser, button state machine,
power policy, firmware cores, and 60 ms Opus encode/decode. The tests synthesize
their inputs and do not need credentials, a relay, or a watch. The power test uses
test-only I/O shims for ESP-IDF timer, Wi-Fi, and I2C calls; it does not measure
hardware power or verify the electrical behavior of the watch. These are host
regressions, not a substitute for an ESP32-S3 build or real-device acceptance.

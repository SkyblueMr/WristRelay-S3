# Audio

This is the device path for the
[Waveshare ESP32-S3-Touch-AMOLED-2.06 V1.0 schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06/ESP32-S3-Touch-AMOLED-2.06.pdf).
It does not use the 2.16-inch board pinout.

- I2C: SDA GPIO15, SCL GPIO14. The ES7210 ADC is at address `0x40`,
  the ES8311 DAC at `0x18`, and the AXP2101 power controller at `0x34`.
- I2S: MCLK GPIO16, BCLK GPIO41, WS GPIO45, playback GPIO40, capture GPIO42.
  The microphone ADC uses MIC1 and MIC2. The active-high speaker enable is
  GPIO46.
- The driver verifies the audio rail configuration at initialization. It
  preserves a pre-enabled 3.3 V ALDO1 and rejects an incompatible voltage.
  Close restores ALDO1 settings if the driver changed them.
- The wire path uses Philips I2S at 24 kHz and 16-bit stereo. RX DMA starts
  at first read; stopping an owner or detecting a capture overrun requires
  the receiver to pause before starting a new stream. RX capture and TX
  playback have separate owners. The amplifier remains muted at initialization
  and can be muted quickly on cancel or after waiting for playback DMA to drain.

The uplink resamples the first microphone lane to 16 kHz using the shared
streaming FIR, then encodes one raw mono Opus packet per 60 ms. The downlink
accepts raw mono 60 ms Opus packets only and decodes them to 24 kHz for playback.
The second microphone lane is available at the device API. This path does
not run acoustic echo cancellation or product-specific wake/voice processing.

The codec is built from the hash-pinned
[Xiph Opus 1.5.2 source release](https://github.com/xiph/opus/blob/main/releases.sha2)
in portable fixed-point mode; no vendor audio binary is used. If the download
is unavailable, pass the matching archive path with `-DWR_OPUS_ARCHIVE=...`.

On ESP32-S3, encode and decode share one DSP-only worker with a 48 KiB PSRAM
stack and bounded PCM/packet storage. This keeps libopus's variable-size scratch
arrays off the smaller network and playback stacks. Each caller gives up after
200 ms waiting for the slot or a further 200 ms waiting for computation. A
computation timeout invalidates the codec instance; the worker cannot write to
caller memory. Both callers must return before closing the codec. Network, flash,
and DMA interrupt work do not run on that worker. This stack reserve is a
conservative starting point, not an on-device high-water measurement.

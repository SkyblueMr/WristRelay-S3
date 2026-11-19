# Board and status display

The display profile targets **Waveshare ESP32-S3-Touch-AMOLED-2.06**, using the
vendor's V1.0 schematic as the wiring reference. This is not the 2.16-inch
board. Check the exact board before running this firmware; disabling the
display does not retarget the remaining hardware.

| Connection | ESP32-S3 GPIO |
| --- | --- |
| CO5300 QSPI clock / chip select | 11 / 12 |
| CO5300 QSPI data lanes 0 to 3 | 4, 5, 6, 7 |
| CO5300 display reset | 8 |
| Shared I2C SDA / SCL | 15 / 14 |

The panel has 410 x 502 visible pixels with a 22-column controller offset.
The status view shows idle, connecting, active audio and error. Its screen
toggle sends panel display on/off commands only. It does not enter MCU
deep sleep, turn off PMIC rails, stop audio, or reconfigure touch.

The display adapter takes no ownership of I2C and never changes AXP2101
configuration. It assumes the board supplies the panel's documented power
rails. Successful SPI transfers do not prove that a powered panel emitted
pixels. Color, panel geometry, button operation and a cold boot on a verified
physical board still require hardware acceptance.

## Public references

- [Waveshare product specifications](https://www.waveshare.com/esp32-s3-touch-amoled-2.06.htm)
- [Waveshare V1.0 schematic](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/tree/30f3b23b3d4bb38d32f7d0cddffb66759668a855/hardware/schematic)
- [Waveshare 2.06 v2.0.0 BSP](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_2_06/versions/2.0.0/readme)
  ([Apache-2.0](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_2_06/versions/2.0.0/license)):
  the [panel initialization table](https://github.com/waveshareteam/Waveshare-ESP32-components/blob/9daffb7168b3c093a330da700988a22092294eef/bsp/esp32_s3_touch_amoled_2_06/esp32_s3_touch_amoled_2_06.c#L49-L60)
  is adapted in `main/display/waveshare_206_init.h` under Apache-2.0. The
  manufacturer BSP uses an SH8601-compatible driver for this CO5300 panel.
  WristRelay uses the dedicated CO5300 driver below and does not bundle or
  link the rest of the BSP.
- [Espressif CO5300 driver v2.2.0](https://components.espressif.com/components/espressif/esp_lcd_co5300/versions/2.2.0/readme)
  ([Apache-2.0](https://components.espressif.com/components/espressif/esp_lcd_co5300/versions/2.2.0/license)):
  built as a separately managed dependency.

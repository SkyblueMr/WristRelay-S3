// SPDX-License-Identifier: Apache-2.0
//
// Adapted from Waveshare Electronics' ESP32-S3-Touch-AMOLED-2.06 BSP v2.0.0:
// https://github.com/waveshareteam/Waveshare-ESP32-components/blob/9daffb7168b3c093a330da700988a22092294eef/bsp/esp32_s3_touch_amoled_2_06/esp32_s3_touch_amoled_2_06.c#L49-L60
// Modifications: use the Espressif CO5300 command type and const payloads,
// omit the unused zero-length payload, and move display-on and brightness to
// the application after frame initialization. See LICENSES/Apache-2.0.txt.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_lcd_co5300.h"

static const co5300_lcd_init_cmd_t wr_waveshare_206_init[] = {
    {0x11, NULL, 0, 120},
    {0xC4, (const uint8_t[]){0x80}, 1, 0},
    {0x44, (const uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (const uint8_t[]){0x00}, 1, 0},
    {0x53, (const uint8_t[]){0x20}, 1, 10},
    {0x63, (const uint8_t[]){0xFF}, 1, 10},
    {0x51, (const uint8_t[]){0x00}, 1, 10},
    {0x2A, (const uint8_t[]){0x00, 0x16, 0x01, 0xAF}, 4, 0},
    {0x2B, (const uint8_t[]){0x00, 0x00, 0x01, 0xF5}, 4, 0},
};

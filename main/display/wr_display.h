// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

typedef enum {
    WR_DISPLAY_IDLE,
    WR_DISPLAY_CONNECTING,
    WR_DISPLAY_AUDIO,
    WR_DISPLAY_ERROR,
} wr_display_status_t;

// Call from one app task. None of these operations changes audio or Wi-Fi.
// The QSPI-only panel does not use the shared I2C bus.
esp_err_t wr_display_init(i2c_master_bus_handle_t bus);
esp_err_t wr_display_status(wr_display_status_t status);
esp_err_t wr_display_set_awake(bool awake);

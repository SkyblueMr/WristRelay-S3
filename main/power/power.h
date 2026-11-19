#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

/* Use the running BSP I2C bus. This module never writes PMIC registers. */
esp_err_t wr_power_supply_init(i2c_master_bus_handle_t bus);
esp_err_t wr_power_battery_only(bool *battery_only);

/* Single-task policy. Pass call_active=true before attempting a call and keep
 * it true through CONNECTING, ACTIVE, and STOPPING. Check the returned error
 * before starting a call. Pass battery_only=false on unknown supply. */
esp_err_t wr_power_step(bool radio_ready, bool battery_only, bool screen_on, bool call_active);

#include "power.h"

#include "esp_timer.h"
#include "esp_wifi.h"

static i2c_master_dev_handle_t pmic;
static int64_t idle_since_us = -1;
static bool mode_known;
static wifi_ps_type_t applied_mode;

esp_err_t wr_power_supply_init(i2c_master_bus_handle_t bus)
{
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pmic) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x34,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(bus, &config, &pmic);
}

static esp_err_t read_pmic_status(uint8_t address, uint8_t *value)
{
    return i2c_master_transmit_receive(pmic, &address, 1, value, 1, 50);
}

esp_err_t wr_power_battery_only(bool *battery_only)
{
    if (!battery_only) {
        return ESP_ERR_INVALID_ARG;
    }
    *battery_only = false;
    if (!pmic) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t id;
    esp_err_t err = read_pmic_status(0x03, &id);
    if (err != ESP_OK) {
        return err;
    }
    if (id != 0x4a) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t status;
    err = read_pmic_status(0x00, &status);
    if (err == ESP_OK) {
        // AXP2101 status: battery present (bit 3) and VBUS good (bit 5).
        *battery_only = (status & 0x28u) == 0x08u;
    }
    return err;
}

esp_err_t wr_power_step(bool radio_ready, bool battery_only, bool screen_on, bool call_active)
{
    if (!radio_ready) {
        idle_since_us = -1;
        mode_known = false;
        return ESP_ERR_INVALID_STATE;
    }
    int64_t now = esp_timer_get_time();
    if (!battery_only || screen_on || call_active) {
        idle_since_us = -1;
    } else if (idle_since_us < 0) {
        idle_since_us = now;
    }
    wifi_ps_type_t desired = idle_since_us >= 0 && now - idle_since_us >= 2000000
                                ? WIFI_PS_MIN_MODEM
                                : WIFI_PS_NONE;
    if (mode_known && applied_mode == desired) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_set_ps(desired);
    mode_known = err == ESP_OK;
    if (mode_known) {
        applied_mode = desired;
    }
    return err;
}

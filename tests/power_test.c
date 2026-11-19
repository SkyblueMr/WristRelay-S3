#include "check.h"
#include "power/power.h"
#include "esp_timer.h"
#include "esp_wifi.h"

struct test_i2c_bus { int unused; };
struct test_i2c_device { int unused; };
static struct test_i2c_bus test_bus;
static struct test_i2c_device test_device;
static uint8_t chip_id = 0x4a;
static uint8_t status;
static bool status_timeout;
static int64_t now_us;
static wifi_ps_type_t last_mode;
static int set_calls;
static esp_err_t wifi_error;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *device)
{
    CHECK(bus == &test_bus);
    CHECK(config->dev_addr_length == I2C_ADDR_BIT_LEN_7);
    CHECK(config->device_address == 0x34);
    *device = &test_device;
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
                                     const uint8_t *write_buffer, size_t write_size,
                                     uint8_t *read_buffer, size_t read_size, int timeout_ms)
{
    CHECK(device == &test_device);
    CHECK(write_size == 1 && read_size == 1 && timeout_ms > 0);
    if (*write_buffer == 0x03) {
        *read_buffer = chip_id;
        return ESP_OK;
    }
    CHECK(*write_buffer == 0x00);
    if (status_timeout) {
        return ESP_ERR_TIMEOUT;
    }
    *read_buffer = status;
    return ESP_OK;
}

int64_t esp_timer_get_time(void)
{
    return now_us;
}

esp_err_t esp_wifi_set_ps(wifi_ps_type_t type)
{
    ++set_calls;
    last_mode = type;
    return wifi_error;
}

static void test_supply(void)
{
    bool battery = true;
    CHECK(wr_power_battery_only(&battery) == ESP_ERR_INVALID_STATE && !battery);
    CHECK(wr_power_supply_init(&test_bus) == ESP_OK);
    status = 0x08;
    CHECK(wr_power_battery_only(&battery) == ESP_OK && battery);
    status = 0x28;
    CHECK(wr_power_battery_only(&battery) == ESP_OK && !battery);
    status = 0x00;
    CHECK(wr_power_battery_only(&battery) == ESP_OK && !battery);
    battery = true;
    status_timeout = true;
    CHECK(wr_power_battery_only(&battery) == ESP_ERR_TIMEOUT && !battery);
    status_timeout = false;
    battery = true;
    chip_id = 0;
    CHECK(wr_power_battery_only(&battery) == ESP_ERR_INVALID_RESPONSE && !battery);
}

static void test_radio(void)
{
    CHECK(wr_power_step(false, true, false, false) == ESP_ERR_INVALID_STATE);
    CHECK(set_calls == 0);

    CHECK(wr_power_step(true, true, false, false) == ESP_OK);
    CHECK(last_mode == WIFI_PS_NONE && set_calls == 1);
    now_us = 1999999;
    CHECK(wr_power_step(true, true, false, false) == ESP_OK);
    CHECK(set_calls == 1);
    now_us = 2000000;
    CHECK(wr_power_step(true, true, false, false) == ESP_OK);
    CHECK(last_mode == WIFI_PS_MIN_MODEM && set_calls == 2);

    wifi_error = ESP_ERR_TIMEOUT;
    CHECK(wr_power_step(true, true, false, true) == ESP_ERR_TIMEOUT);
    CHECK(last_mode == WIFI_PS_NONE && set_calls == 3);
    wifi_error = ESP_OK;
    CHECK(wr_power_step(true, true, false, true) == ESP_OK);
    CHECK(last_mode == WIFI_PS_NONE && set_calls == 4);
    CHECK(wr_power_step(true, true, false, false) == ESP_OK);
    CHECK(set_calls == 4);
    now_us = 4000000;
    CHECK(wr_power_step(true, true, false, false) == ESP_OK);
    CHECK(last_mode == WIFI_PS_MIN_MODEM && set_calls == 5);

    CHECK(wr_power_step(true, true, true, false) == ESP_OK);
    CHECK(last_mode == WIFI_PS_NONE && set_calls == 6);
    CHECK(wr_power_step(true, false, false, false) == ESP_OK);
    now_us = 8000000;
    CHECK(wr_power_step(true, false, false, false) == ESP_OK);
    CHECK(set_calls == 6);
    CHECK(wr_power_step(false, true, false, false) == ESP_ERR_INVALID_STATE);
    CHECK(wr_power_step(true, true, false, false) == ESP_OK);
    CHECK(last_mode == WIFI_PS_NONE && set_calls == 7);
}

int main(void)
{
    test_supply();
    test_radio();
    return 0;
}

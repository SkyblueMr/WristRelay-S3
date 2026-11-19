#include "wake.h"

#include <stdatomic.h>
#include "button_logic.h"
#include "driver/gpio.h"
#include "esp_timer.h"

static wr_button_t user_key;
static bool initialized;
static atomic_bool acoustic_pending;

esp_err_t wr_wake_init(void)
{
    /* AMOLED 2.06 BOOT is active-low. GPIO0 is a strap; input after boot only. */
    gpio_config_t input = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&input);
    if (err != ESP_OK) {
        return err;
    }
    wr_button_init(&user_key, gpio_get_level(GPIO_NUM_0) == 0, esp_timer_get_time());
    initialized = true;
    return ESP_OK;
}

wr_wake_event_t wr_wake_poll(void)
{
    if (!initialized) {
        return WR_WAKE_NONE;
    }
    wr_button_event_t event = wr_button_step(&user_key, gpio_get_level(GPIO_NUM_0) == 0,
                                            esp_timer_get_time());
    if (event == WR_BUTTON_LONG) {
        atomic_store(&acoustic_pending, false);
        return WR_WAKE_VOICE_TOGGLE;
    }
    if (event == WR_BUTTON_SHORT) {
        return WR_WAKE_SCREEN_TOGGLE;
    }
    return atomic_exchange(&acoustic_pending, false) ? WR_WAKE_ACOUSTIC : WR_WAKE_NONE;
}

void wr_wake_request_acoustic(void)
{
    atomic_store(&acoustic_pending, true);
}

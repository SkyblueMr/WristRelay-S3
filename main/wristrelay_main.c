// SPDX-License-Identifier: MIT

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "power.h"
#include "wake.h"
#include "wr_audio.h"
#include "wr_display.h"
#include "wr_voice.h"
#ifdef CONFIG_WR_WAKE_WAKENET
#include "wakenet.h"
#endif

static const char *const TAG = "WristRelay";
static EventGroupHandle_t network_events;

#define NET_HAS_IP BIT0
#define NET_TIME_SET BIT1
#define NET_RECONNECT BIT2

static void net_event(void *context, esp_event_base_t base, int32_t event, void *data)
{
    (void)context;
    if (base == WIFI_EVENT && (event == WIFI_EVENT_STA_START || event == WIFI_EVENT_STA_DISCONNECTED)) {
        xEventGroupClearBits(network_events, NET_HAS_IP);
        xEventGroupSetBits(network_events, NET_RECONNECT);
    } else if (base == IP_EVENT && event == IP_EVENT_STA_GOT_IP) {
        xEventGroupClearBits(network_events, NET_RECONNECT);
        xEventGroupSetBits(network_events, NET_HAS_IP);
        esp_err_t err = esp_netif_sntp_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not start clock sync: %s", esp_err_to_name(err));
        }
    } else if (base == IP_EVENT && event == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(network_events, NET_HAS_IP);
    } else if (base == NETIF_SNTP_EVENT && event == NETIF_SNTP_TIME_SYNC) {
        const esp_netif_sntp_time_sync_t *sync = data;
        if (sync && sync->tv.tv_sec >= 1704067200) {  // 2024-01-01 UTC
            xEventGroupSetBits(network_events, NET_TIME_SET);
        } else {
            xEventGroupClearBits(network_events, NET_TIME_SET);
            ESP_LOGE(TAG, "Time server returned an invalid date");
        }
    }
}

static esp_err_t start_wifi(void)
{
    const char *ssid = CONFIG_WR_WIFI_SSID;
    const char *passphrase = CONFIG_WR_WIFI_PASSPHRASE;
    wifi_config_t config = {0};
    size_t ssid_length = strlen(ssid);
    size_t pass_length = strlen(passphrase);
    if (ssid_length == 0 || ssid_length > sizeof(config.sta.ssid) ||
        (pass_length && (pass_length < 8 || pass_length >= sizeof(config.sta.password))) ||
        !CONFIG_WR_SNTP_SERVER[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(config.sta.ssid, ssid, ssid_length);
    memcpy(config.sta.password, passphrase, pass_length);
    config.sta.threshold.authmode = pass_length ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;

    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "NVS initialization failed; flash not erased");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Network initialization failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Network event loop initialization failed");
    if (!esp_netif_create_default_wifi_sta()) {
        return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi initialization failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Could not select Wi-Fi RAM storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Could not select Wi-Fi station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "Invalid Wi-Fi settings");

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_WR_SNTP_SERVER);
    sntp.start = false;
    sntp.wait_for_sync = false;
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&sntp), TAG, "Clock sync initialization failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, net_event, NULL),
                        TAG, "Could not register Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, net_event, NULL),
                        TAG, "Could not register IP events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC, net_event, NULL),
                        TAG, "Could not register clock sync events");
    return esp_wifi_start();
}

static bool call_in_progress(wr_voice_state_t state)
{
    return state == WR_VOICE_CONNECTING || state == WR_VOICE_ACTIVE || state == WR_VOICE_STOPPING;
}

#ifdef CONFIG_WR_DISPLAY_WAVESHARE_206
static void show_state(wr_voice_state_t state)
{
    wr_display_status_t status = WR_DISPLAY_IDLE;
    switch (state) {
        case WR_VOICE_CONNECTING:
        case WR_VOICE_STOPPING: status = WR_DISPLAY_CONNECTING; break;
        case WR_VOICE_ACTIVE: status = WR_DISPLAY_AUDIO; break;
        case WR_VOICE_ERROR: status = WR_DISPLAY_ERROR; break;
        default: break;
    }
    esp_err_t err = wr_display_status(status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not update panel status: %s", esp_err_to_name(err));
    }
}
#endif

void app_main(void)
{
    wr_audio_t *audio = NULL;
    esp_err_t err = wr_audio_open(&audio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Audio initialization failed: %s", esp_err_to_name(err));
        return;
    }
    err = wr_wake_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Control initialization failed: %s", esp_err_to_name(err));
        wr_audio_close(&audio);
        return;
    }
    i2c_master_bus_handle_t i2c_bus = wr_audio_i2c_bus(audio);
    err = wr_power_supply_init(i2c_bus);
    bool power_monitor_ready = err == ESP_OK;
    if (!power_monitor_ready) {
        ESP_LOGW(TAG, "Power monitoring unavailable: %s", esp_err_to_name(err));
    }

    network_events = xEventGroupCreate();
    if (!network_events) {
        ESP_LOGE(TAG, "Could not allocate network event group");
        return;
    }
    bool wifi_started = false;
    if (CONFIG_WR_WIFI_SSID[0]) {
        err = start_wifi();
        wifi_started = err == ESP_OK;
        if (!wifi_started) {
            ESP_LOGE(TAG, "Wi-Fi did not start: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "Wi-Fi is not configured");
    }

    bool battery_only = false;
    bool screen_on = false;
#ifdef CONFIG_WR_DISPLAY_WAVESHARE_206
    err = wr_display_init(i2c_bus);
    bool panel_ready = err == ESP_OK;
    screen_on = panel_ready;
    wr_voice_state_t shown_state = (wr_voice_state_t)-1;
    if (!panel_ready) {
        ESP_LOGE(TAG, "Display initialization failed: %s", esp_err_to_name(err));
    }
#endif
    int64_t reconnect_at = 0;
    int64_t power_sample_at = 0;
    esp_err_t previous_ps_error = ESP_OK;
    esp_err_t previous_supply_error = ESP_OK;
#ifdef CONFIG_WR_WAKE_WAKENET
    err = wr_wakenet_init();
    bool wakenet_ready = err == ESP_OK;
    bool wake_owned_rx = false;
    esp_err_t previous_wake_error = ESP_OK;
    if (!wakenet_ready) {
        ESP_LOGE(TAG, "On-device acoustic wake unavailable: %s", esp_err_to_name(err));
    }
#endif
    for (;;) {
        int64_t now = esp_timer_get_time();
        EventBits_t network = xEventGroupGetBits(network_events);
        if (wifi_started && (network & NET_RECONNECT) && now >= reconnect_at) {
            reconnect_at = now + 3000000;
            xEventGroupClearBits(network_events, NET_RECONNECT);
            err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Wi-Fi connection could not start: %s", esp_err_to_name(err));
                xEventGroupSetBits(network_events, NET_RECONNECT);
            }
        }
        const bool network_ready = (network & (NET_HAS_IP | NET_TIME_SET)) == (NET_HAS_IP | NET_TIME_SET);
        wr_voice_state_t state = wr_voice_state();
        bool active = call_in_progress(state);
        if (!network_ready && (state == WR_VOICE_CONNECTING || state == WR_VOICE_ACTIVE)) {
            wr_voice_network_lost();
        }
        if (power_monitor_ready && now >= power_sample_at) {
            power_sample_at = now + 1000000;
            err = wr_power_battery_only(&battery_only);
            if (err != ESP_OK) {
                battery_only = false;
                if (err != previous_supply_error) {
                    ESP_LOGW(TAG, "Could not read power source: %s", esp_err_to_name(err));
                }
            }
            previous_supply_error = err;
        }
        wr_wake_event_t event = wr_wake_poll();
        bool polled_audio = false;
#ifdef CONFIG_WR_WAKE_WAKENET
        if (wakenet_ready && !active && network_ready && CONFIG_WR_VOICE_URI[0] && event == WR_WAKE_NONE) {
            bool matched = false;
            err = wr_wakenet_poll(audio, &matched);
            wake_owned_rx = true;
            if (err != ESP_OK && err != previous_wake_error) {
                ESP_LOGW(TAG, "Acoustic detection failed: %s", esp_err_to_name(err));
            }
            previous_wake_error = err;
            if (err == ESP_OK) {
                polled_audio = true;
                if (matched) {
                    event = WR_WAKE_ACOUSTIC;
                }
            }
        } else {
            wr_wakenet_pause();
        }
        if (wake_owned_rx && !active && (event != WR_WAKE_NONE || !network_ready)) {
            err = wr_audio_pause_rx(audio);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Could not pause local microphone capture: %s", esp_err_to_name(err));
            } else {
                wake_owned_rx = false;
            }
        }
#endif
#ifdef CONFIG_WR_DISPLAY_WAVESHARE_206
        if (panel_ready && event == WR_WAKE_SCREEN_TOGGLE) {
            err = wr_display_set_awake(!screen_on);
            if (err == ESP_OK) {
                screen_on = !screen_on;
                if (screen_on) {
                    shown_state = (wr_voice_state_t)-1;
                }
            } else {
                ESP_LOGW(TAG, "Could not toggle display: %s", esp_err_to_name(err));
            }
        }
        if (panel_ready && screen_on && shown_state != state) {
            show_state(state);
            shown_state = state;
        }
#endif
        if (event == WR_WAKE_VOICE_TOGGLE && active) {
            wr_voice_stop();
        } else if ((event == WR_WAKE_VOICE_TOGGLE || event == WR_WAKE_ACOUSTIC) && !active) {
            if (!network_ready || !CONFIG_WR_VOICE_URI[0]) {
                ESP_LOGW(TAG, "Voice unavailable: requires Wi-Fi, clock sync, and a relay URL");
            } else {
#ifdef CONFIG_WR_WAKE_WAKENET
                wr_wakenet_pause();
                if (wake_owned_rx) {
                    ESP_LOGE(TAG, "Call not started because local microphone capture did not release");
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
#endif
                err = wr_power_step(true, battery_only, screen_on, true);
                if (err == ESP_OK) {
                    err = wr_voice_start(audio, CONFIG_WR_VOICE_URI, CONFIG_WR_VOICE_TOKEN);
                    active = err == ESP_OK;
                }
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Call could not start: %s", esp_err_to_name(err));
                }
            }
        }
        err = wr_power_step(wifi_started, battery_only, screen_on, active);
        if (err != ESP_OK && err != previous_ps_error) {
            ESP_LOGW(TAG, "Wi-Fi power mode could not be applied: %s", esp_err_to_name(err));
        }
        previous_ps_error = err;
        vTaskDelay(polled_audio ? 1 : pdMS_TO_TICKS(10));
    }
}

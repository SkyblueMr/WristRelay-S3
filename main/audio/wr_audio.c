#include "wr_audio.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Waveshare ESP32-S3-Touch-AMOLED-2.06 schematic V1.0 (not the 2.16 board). */
enum { I2S_DMA_COUNT = 4, I2S_DMA_FRAMES = 240, I2S_MCLK_MULTIPLE = 512 };
static const gpio_num_t speaker_enable_pin = GPIO_NUM_46;
static const char *const tag = "wr_audio";

typedef struct {
    audio_codec_ctrl_if_t api;
    const audio_codec_ctrl_if_t *driver;
    int first_error;
} codec_control_t;

struct wr_audio {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t pmu;
    i2s_chan_handle_t tx;
    i2s_chan_handle_t rx;
    codec_control_t adc_control;
    codec_control_t dac_control;
    const audio_codec_if_t *adc;
    const audio_codec_if_t *dac;
    portMUX_TYPE dma_lock;
    uint32_t dma_completed;
    uint32_t last_play_dma;
    bool rx_overrun;
    int16_t tx_pcm[WR_AUDIO_FRAMES * 2];
    bool pa_ready;
    bool pa_enabled;
    bool tx_on;
    bool rx_on;
    bool ready;
    bool pending_playback;
    bool override_enable;
    bool override_voltage;
    uint8_t original_voltage;
};

static esp_err_t pmu_read(wr_audio_t *a, uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(a->pmu, &reg, 1, value, 1, 100);
}

static esp_err_t pmu_change(wr_audio_t *a, uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t old = 0;
    esp_err_t err = pmu_read(a, reg, &old);
    if (err != ESP_OK || (old & mask) == value) {
        return err;
    }
    uint8_t bytes[] = {reg, (old & ~mask) | value};
    err = i2c_master_transmit(a->pmu, bytes, sizeof(bytes), 100);
    if (err != ESP_OK) {
        return err;
    }
    err = pmu_read(a, reg, &old);
    return err == ESP_OK && (old & mask) != value ? ESP_ERR_INVALID_RESPONSE : err;
}

static esp_err_t enable_audio_rail(wr_audio_t *a)
{
    const i2c_device_config_t pmu = {.device_address = 0x34, .scl_speed_hz = 100000};
    esp_err_t err = i2c_master_bus_add_device(a->bus, &pmu, &a->pmu);
    uint8_t chip = 0, enable = 0, voltage = 0;
    if (err == ESP_OK) {
        err = pmu_read(a, 0x03, &chip);
    }
    if (err != ESP_OK || chip != 0x4a) {
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }
    err = pmu_read(a, 0x90, &enable);
    if (err == ESP_OK) {
        err = pmu_read(a, 0x92, &voltage);
    }
    if (err != ESP_OK) {
        return err;
    }
    /* Never change an already-powered rail's voltage. ALDO1 0x1c = 3.3 V. */
    a->original_voltage = voltage & 0x1f;
    if (enable & 1) {
        return a->original_voltage == 0x1c ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (a->original_voltage != 0x1c) {
        a->override_voltage = true;
        err = pmu_change(a, 0x92, 0x1f, 0x1c);
    }
    if (err == ESP_OK) {
        a->override_enable = true;
        err = pmu_change(a, 0x90, 1, 1);
    }
    return err;
}

/* Some codec setters do not propagate an I2C error. Latch and stop the rest
 * of that programming sequence on the first control operation failure. */
static bool control_open(const audio_codec_ctrl_if_t *api)
{
    const codec_control_t *c = (const void *)api;
    return c->driver && c->driver->is_open(c->driver);
}

static int control_read(const audio_codec_ctrl_if_t *api, int reg, int reglen, void *out, int len)
{
    codec_control_t *c = (void *)api;
    if (!c->first_error) {
        c->first_error = c->driver->read_reg(c->driver, reg, reglen, out, len);
    }
    if (c->first_error) {
        memset(out, 0, (size_t)len);
    }
    return c->first_error;
}

static int control_write(const audio_codec_ctrl_if_t *api, int reg, int reglen, void *in, int len)
{
    codec_control_t *c = (void *)api;
    if (!c->first_error) {
        c->first_error = c->driver->write_reg(c->driver, reg, reglen, in, len);
    }
    return c->first_error;
}

static esp_err_t open_control(wr_audio_t *a, codec_control_t *control, uint8_t addr)
{
    /* esp_codec_dev uses 8-bit addresses; the I2C bus API uses 7-bit addresses. */
    esp_err_t err = i2c_master_probe(a->bus, addr >> 1, 100);
    if (err != ESP_OK) {
        return err;
    }
    audio_codec_i2c_cfg_t cfg = {.port = I2C_NUM_0, .addr = addr, .bus_handle = a->bus};
    control->driver = audio_codec_new_i2c_ctrl(&cfg);
    control->api = (audio_codec_ctrl_if_t){
        .is_open = control_open,
        .read_reg = control_read,
        .write_reg = control_write
    };
    return control->driver ? ESP_OK : ESP_FAIL;
}

static esp_err_t check_codec(codec_control_t *control, int result)
{
    if (control->first_error || result) {
        ESP_LOGE(tag, "codec control failed (i2c=%d, codec=%d)", control->first_error, result);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool readback(const audio_codec_if_t *codec, int reg, int mask, int expected)
{
    int value = 0;
    return codec->get_reg(codec, reg, &value) == 0 && (value & mask) == expected;
}

static esp_err_t set_amp(wr_audio_t *a, bool enabled)
{
    esp_err_t err = gpio_set_level(speaker_enable_pin, enabled);
    if (err == ESP_OK && gpio_get_level(speaker_enable_pin) != enabled) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        a->pa_enabled = enabled;
    }
    return err;
}

static esp_err_t set_dac_mute(wr_audio_t *a, bool mute)
{
    esp_err_t err = check_codec(&a->dac_control, a->dac->mute(a->dac, mute));
    if (err == ESP_OK && !readback(a->dac, 0x31, 0x60, mute ? 0x60 : 0)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    return err;
}

static esp_err_t open_codecs(wr_audio_t *a)
{
    esp_err_t err = open_control(a, &a->adc_control, ES7210_CODEC_DEFAULT_ADDR);
    if (err != ESP_OK) {
        return err;
    }
    err = open_control(a, &a->dac_control, ES8311_CODEC_DEFAULT_ADDR);
    if (err != ESP_OK) {
        return err;
    }
    es7210_codec_cfg_t adc = {
        .ctrl_if = &a->adc_control.api,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
        .mclk_div = I2S_MCLK_MULTIPLE
    };
    es8311_codec_cfg_t dac = {
        .ctrl_if = &a->dac_control.api,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,
        .use_mclk = true,
        .mclk_div = I2S_MCLK_MULTIPLE
    };
    a->adc = es7210_codec_new(&adc);
    a->dac = es8311_codec_new(&dac);
    if (!a->adc || !a->dac || a->adc_control.first_error || a->dac_control.first_error) {
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t format = {
        .sample_rate = WR_AUDIO_RATE, .bits_per_sample = 16, .channel = 2,
        .mclk_multiple = I2S_MCLK_MULTIPLE
    };
    if (check_codec(&a->adc_control, a->adc->set_fs(a->adc, &format)) ||
        check_codec(&a->adc_control, a->adc->enable(a->adc, true)) ||
        check_codec(&a->adc_control, a->adc->set_mic_gain(a->adc, 24.0f)) ||
        check_codec(&a->adc_control, a->adc->mute_mic(a->adc, false))) {
        return ESP_FAIL;
    }
    /* The 1.5.11 ES7210 driver skips ADC clock setup in slave mode.
     * At 12.288 MHz/24 kHz, use its 1x coefficient without touching
     * the master-only WS divider. Check slave/pad/OSR first. */
    if (!readback(a->adc, 0x08, 3, 0) || !readback(a->adc, 0x03, 0x80, 0) ||
        !readback(a->adc, 0x07, 0xff, 0x20) ||
        check_codec(&a->adc_control, a->adc->set_reg(a->adc, 0x02, 0x81)) ||
        !readback(a->adc, 0x02, 0xff, 0x81) ||
        !readback(a->adc, 0x08, 3, 0) ||
        !readback(a->adc, 0x14, 3, 0) || !readback(a->adc, 0x15, 3, 0) ||
        !readback(a->adc, 0x43, 0x1f, 0x18) || !readback(a->adc, 0x44, 0x1f, 0x18)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (check_codec(&a->dac_control, a->dac->set_fs(a->dac, &format)) ||
        check_codec(&a->dac_control, a->dac->enable(a->dac, true)) ||
        check_codec(&a->dac_control, a->dac->set_reg(a->dac, 0x32, 0xbf)) ||
        !readback(a->dac, 0x32, 0xff, 0xbf)) {
        return ESP_FAIL;
    }
    return set_dac_mute(a, true);
}

static bool IRAM_ATTR tx_done(i2s_chan_handle_t channel, i2s_event_data_t *event, void *arg)
{
    (void)channel;
    (void)event;
    wr_audio_t *a = arg;
    portENTER_CRITICAL_ISR(&a->dma_lock);
    ++a->dma_completed;
    portEXIT_CRITICAL_ISR(&a->dma_lock);
    return false;
}

static uint32_t tx_completed(wr_audio_t *a)
{
    portENTER_CRITICAL(&a->dma_lock);
    uint32_t done = a->dma_completed;
    portEXIT_CRITICAL(&a->dma_lock);
    return done;
}

static bool IRAM_ATTR rx_overflow(i2s_chan_handle_t channel, i2s_event_data_t *event, void *arg)
{
    (void)channel;
    (void)event;
    wr_audio_t *a = arg;
    portENTER_CRITICAL_ISR(&a->dma_lock);
    a->rx_overrun = true;
    portEXIT_CRITICAL_ISR(&a->dma_lock);
    return false;
}

static bool rx_discontinuous(wr_audio_t *a)
{
    portENTER_CRITICAL(&a->dma_lock);
    bool missed = a->rx_overrun;
    portEXIT_CRITICAL(&a->dma_lock);
    return missed;
}

static esp_err_t open_i2s(wr_audio_t *a)
{
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = I2S_DMA_COUNT;
    channel.dma_frame_num = I2S_DMA_FRAMES;
    channel.auto_clear_after_cb = true;
    esp_err_t err = i2s_new_channel(&channel, &a->tx, &a->rx);
    if (err != ESP_OK) {
        return err;
    }
    i2s_std_config_t stream = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(WR_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_16, .bclk = GPIO_NUM_41, .ws = GPIO_NUM_45,
            .dout = GPIO_NUM_40, .din = GPIO_NUM_42
        }
    };
    stream.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_512;
    err = i2s_channel_init_std_mode(a->tx, &stream);
    if (err == ESP_OK) {
        err = i2s_channel_init_std_mode(a->rx, &stream);
    }
    if (err == ESP_OK) {
        const i2s_event_callbacks_t callbacks = {.on_sent = tx_done};
        err = i2s_channel_register_event_callback(a->tx, &callbacks, a);
    }
    if (err == ESP_OK) {
        const i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = rx_overflow};
        err = i2s_channel_register_event_callback(a->rx, &callbacks, a);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(a->tx);
        a->tx_on = err == ESP_OK;
    }
    return err;
}

esp_err_t wr_audio_open(wr_audio_t **out)
{
    if (!out || *out) {
        return ESP_ERR_INVALID_ARG;
    }
    /* DMA ISR user data must be accessible even when PSRAM cache is off. */
    wr_audio_t *a = heap_caps_calloc(1, sizeof(*a), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!a) {
        return ESP_ERR_NO_MEM;
    }
    a->dma_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    *out = a;
    const gpio_config_t pa = {.pin_bit_mask = 1ULL << speaker_enable_pin, .mode = GPIO_MODE_INPUT_OUTPUT};
    esp_err_t err = gpio_set_level(speaker_enable_pin, 0);
    if (err == ESP_OK) {
        err = gpio_config(&pa);
        a->pa_ready = err == ESP_OK;
    }
    if (err == ESP_OK) {
        err = set_amp(a, false);
    }
    if (err == ESP_OK) {
        const i2c_master_bus_config_t bus = {
            .i2c_port = I2C_NUM_0, .scl_io_num = GPIO_NUM_14, .sda_io_num = GPIO_NUM_15,
            .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7
        };
        err = i2c_new_master_bus(&bus, &a->bus);
    }
    if (err == ESP_OK) {
        err = enable_audio_rail(a);
    }
    if (err == ESP_OK) {
        err = open_i2s(a);
    }
    if (err == ESP_OK) {
        err = open_codecs(a);
    }
    if (err == ESP_OK) {
        a->ready = true;
    }
    if (err != ESP_OK) {
        esp_err_t cleanup = wr_audio_close(out);
        ESP_LOGE(tag, "audio start failed: %s; teardown: %s",
                 esp_err_to_name(err), esp_err_to_name(cleanup));
    }
    return err;
}

i2c_master_bus_handle_t wr_audio_i2c_bus(wr_audio_t *a)
{
    return a && a->ready ? a->bus : NULL;
}

esp_err_t wr_audio_read_frames(wr_audio_t *a, int16_t *stereo, size_t frames, unsigned timeout_ms)
{
    if (!a || !stereo || !frames || frames > WR_AUDIO_FRAMES || !timeout_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!a->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!a->rx_on) {
        esp_err_t err = i2s_channel_enable(a->rx);
        if (err != ESP_OK) {
            return err;
        }
        a->rx_on = true;
    }
    if (rx_discontinuous(a)) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t count = 0;
    size_t expected = frames * 2 * sizeof(*stereo);
    esp_err_t err = i2s_channel_read(a->rx, stereo, expected, &count, timeout_ms);
    if (err == ESP_OK && rx_discontinuous(a)) {
        return ESP_ERR_INVALID_STATE;
    }
    return err == ESP_OK && count != expected ? ESP_ERR_INVALID_SIZE : err;
}

esp_err_t wr_audio_read(wr_audio_t *a, int16_t stereo[WR_AUDIO_FRAMES * 2], unsigned timeout_ms)
{
    return wr_audio_read_frames(a, stereo, WR_AUDIO_FRAMES, timeout_ms);
}

esp_err_t wr_audio_pause_rx(wr_audio_t *a)
{
    if (!a || !a->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!a->rx_on) {
        return ESP_OK;
    }
    esp_err_t err = i2s_channel_disable(a->rx);
    if (err == ESP_OK) {
        a->rx_on = false;
        portENTER_CRITICAL(&a->dma_lock);
        a->rx_overrun = false;
        portEXIT_CRITICAL(&a->dma_lock);
    }
    return err;
}

esp_err_t wr_audio_drain(wr_audio_t *a, unsigned timeout_ms)
{
    if (!a || !timeout_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!a->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t start = xTaskGetTickCount();
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    while (a->pending_playback && tx_completed(a) - a->last_play_dma < I2S_DMA_COUNT) {
        if (xTaskGetTickCount() - start >= ticks) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    a->pending_playback = false;
    return ESP_OK;
}

esp_err_t wr_audio_mute(wr_audio_t *a)
{
    if (!a) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = a->pa_ready ? set_amp(a, false) : ESP_OK;
    if (a->dac) {
        a->dac_control.first_error = 0;
        esp_err_t mute = set_dac_mute(a, true);
        if (err == ESP_OK) {
            err = mute;
        }
    }
    return err;
}

esp_err_t wr_audio_play(wr_audio_t *a, const int16_t *mono, size_t frames, unsigned timeout_ms)
{
    if (!a || !mono || !frames || frames > WR_AUDIO_FRAMES || !timeout_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!a->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    if (!a->pa_enabled) {
        /* Canceled samples must not become audible on a subsequent playback. */
        err = wr_audio_drain(a, timeout_ms);
        if (err == ESP_OK) {
            a->dac_control.first_error = 0;
            err = set_dac_mute(a, false);
        }
        if (err == ESP_OK) {
            err = set_amp(a, true);
        }
    }
    if (err == ESP_OK) {
        for (size_t i = 0; i < frames; ++i) {
            /* Keep an explicit output headroom limit with no hidden software gain. */
            int16_t sample = mono[i] > 16384 ? 16384 : mono[i] < -16384 ? -16384 : mono[i];
            a->tx_pcm[i * 2] = sample;
            a->tx_pcm[i * 2 + 1] = sample;
        }
        size_t count = 0, expected = frames * 2 * sizeof(*mono);
        err = i2s_channel_write(a->tx, a->tx_pcm, expected, &count, timeout_ms);
        a->last_play_dma = tx_completed(a);
        a->pending_playback = true;
        if (err == ESP_OK && count != expected) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    if (err != ESP_OK) {
        esp_err_t mute = wr_audio_mute(a);
        if (mute != ESP_OK) {
            ESP_LOGE(tag, "playback failed (%s); mute failed (%s)",
                     esp_err_to_name(err), esp_err_to_name(mute));
        }
    }
    return err;
}

static void mark_error(esp_err_t *first, esp_err_t err)
{
    if (*first == ESP_OK && err != ESP_OK) {
        *first = err;
    }
}

static void close_codec(const audio_codec_if_t **codec, codec_control_t *control, esp_err_t *first)
{
    if (*codec) {
        control->first_error = 0;
        mark_error(first, check_codec(control, (*codec)->enable(*codec, false)));
        control->first_error = 0;
        mark_error(first, check_codec(control, audio_codec_delete_codec_if(*codec)));
        *codec = NULL;
    }
    if (control->driver) {
        mark_error(first, audio_codec_delete_ctrl_if(control->driver) == 0 ? ESP_OK : ESP_FAIL);
        control->driver = NULL;
    }
}

static void close_channel(i2s_chan_handle_t *channel, bool *running, esp_err_t *first)
{
    if (*running) {
        esp_err_t err = i2s_channel_disable(*channel);
        mark_error(first, err);
        if (err != ESP_OK) {
            return;
        }
        *running = false;
    }
    if (*channel) {
        esp_err_t err = i2s_del_channel(*channel);
        mark_error(first, err);
        if (err == ESP_OK) {
            *channel = NULL;
        }
    }
}

esp_err_t wr_audio_close(wr_audio_t **handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    wr_audio_t *a = *handle;
    if (!a) {
        return ESP_OK;
    }
    a->ready = false;
    esp_err_t first = wr_audio_mute(a);
    close_codec(&a->dac, &a->dac_control, &first);
    close_codec(&a->adc, &a->adc_control, &first);
    close_channel(&a->rx, &a->rx_on, &first);
    close_channel(&a->tx, &a->tx_on, &first);
    if (a->pmu && a->override_enable) {
        esp_err_t err = pmu_change(a, 0x90, 1, 0);
        mark_error(&first, err);
        a->override_enable = err != ESP_OK;
    }
    if (a->pmu && !a->override_enable && a->override_voltage) {
        esp_err_t err = pmu_change(a, 0x92, 0x1f, a->original_voltage);
        mark_error(&first, err);
        a->override_voltage = err != ESP_OK;
    }
    if (a->pmu && !a->override_enable && !a->override_voltage) {
        esp_err_t err = i2c_master_bus_rm_device(a->pmu);
        mark_error(&first, err);
        if (err == ESP_OK) {
            a->pmu = NULL;
        }
    }
    if (a->bus && !a->pmu) {
        esp_err_t err = i2c_del_master_bus(a->bus);
        mark_error(&first, err);
        if (err == ESP_OK) {
            a->bus = NULL;
        }
    }
    if (!a->tx && !a->rx && !a->bus && !a->pa_enabled) {
        free(a);
        *handle = NULL;
    }
    return first;
}

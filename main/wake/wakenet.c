#include "wakenet.h"

#include "sdkconfig.h"

#ifdef CONFIG_WR_WAKE_WAKENET
#include <stdlib.h>
#include <string.h>

#include "board_audio_resampler.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model_path.h"

#define MODEL_NAME "wn9_hiesp"
#define MICROPHONE_RATE 24000
#define MODEL_RATE 16000
#define MAX_CAPTURE_FRAMES 1440
#define WAKE_MAIN_STACK_BYTES 8192

_Static_assert(CONFIG_ESP_MAIN_TASK_STACK_SIZE >= WAKE_MAIN_STACK_BYTES,
               "WakeNet requires at least 8192 bytes of app_main stack");

static srmodel_list_t *model_list;
static const esp_wn_iface_t *detector;
static model_iface_data_t *detector_data;
static board_resampler_t resampler;
static int16_t *capture;
static int16_t *input;
static size_t capture_frames;
static size_t model_samples;
static bool paused = true;
static unsigned settling_frames;
static bool init_attempted;
static bool stack_reported;

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static esp_err_t check_model_partition(const esp_partition_t *partition)
{
    if (!partition) {
        return ESP_ERR_NOT_FOUND;
    }
    // ESP-SR trusts its packed directory; reject blank or malformed flash first.
    uint8_t count_bytes[4];
    esp_err_t err = esp_partition_read(partition, 0, count_bytes, sizeof(count_bytes));
    if (err != ESP_OK) {
        return err;
    }
    uint32_t count = read_le32(count_bytes);
    if (count == 0 || count == UINT32_MAX) {
        return ESP_ERR_NOT_FOUND;
    }
    if (count > 16) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t cursor = sizeof(count_bytes);
    bool found = false;
    uint32_t first_payload = partition->size;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t model_header[SRMODEL_STRING_LENGTH + 4];
        err = esp_partition_read(partition, cursor, model_header, sizeof(model_header));
        if (err != ESP_OK) {
            return err;
        }
        cursor += sizeof(model_header);
        if (!memchr(model_header, '\0', SRMODEL_STRING_LENGTH)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        bool selected = strcmp((const char *)model_header, MODEL_NAME) == 0;
        uint32_t files = read_le32(model_header + SRMODEL_STRING_LENGTH);
        if (files == 0 || files > 16) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        bool info = false;
        bool data = false;
        bool index = false;
        for (uint32_t j = 0; j < files; ++j) {
            uint8_t file_header[SRMODEL_STRING_LENGTH + 8];
            err = esp_partition_read(partition, cursor, file_header, sizeof(file_header));
            if (err != ESP_OK) {
                return err;
            }
            cursor += sizeof(file_header);
            uint32_t offset = read_le32(file_header + SRMODEL_STRING_LENGTH);
            uint32_t length = read_le32(file_header + SRMODEL_STRING_LENGTH + 4);
            if (!memchr(file_header, '\0', SRMODEL_STRING_LENGTH) || offset > partition->size ||
                !length || length > partition->size - offset) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (selected) {
                const char *file_name = (const char *)file_header;
                info |= strcmp(file_name, "_MODEL_INFO_") == 0;
                data |= strcmp(file_name, "wn9_data") == 0;
                index |= strcmp(file_name, "wn9_index") == 0;
            }
            if (offset < first_payload) {
                first_payload = offset;
            }
        }
        if (selected) {
            if (found || !info || !data || !index) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            found = true;
        }
    }
    if (first_payload < cursor) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t wr_wakenet_init(void)
{
    if (init_attempted) {
        return ESP_ERR_INVALID_STATE;
    }
    init_attempted = true;
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
    esp_err_t err = check_model_partition(partition);
    if (err != ESP_OK) {
        return err;
    }
    model_list = esp_srmodel_init("model");
    if (!model_list) {
        return ESP_ERR_NOT_FOUND;
    }
    err = ESP_ERR_NOT_FOUND;
    char *name = esp_srmodel_filter(model_list, ESP_WN_PREFIX, MODEL_NAME);
    if (!name || strcmp(name, MODEL_NAME) != 0) {
        goto fail;
    }
    detector = esp_wn_handle_from_name(name);
    if (!detector) {
        goto fail;
    }
    err = ESP_FAIL;
    detector_data = detector->create(name, DET_MODE_90);
    if (!detector_data) {
        goto fail;
    }
    int samples = detector->get_samp_chunksize(detector_data);
    err = ESP_ERR_NOT_SUPPORTED;
    if (detector->get_samp_rate(detector_data) != MODEL_RATE ||
        detector->get_channel_num(detector_data) != 1 || samples <= 0 ||
        samples > MAX_CAPTURE_FRAMES * 2 / 3 || samples % 2 != 0 ||
        board_resampler_init(&resampler, MICROPHONE_RATE, MODEL_RATE) != 0) {
        goto fail;
    }
    model_samples = (size_t)samples;
    capture_frames = model_samples * 3 / 2;
    err = ESP_ERR_NO_MEM;
    capture = malloc(capture_frames * 2 * sizeof(*capture));
    input = malloc(model_samples * sizeof(*input));
    if (!capture || !input) {
        goto fail;
    }
    return ESP_OK;

fail:
    free(capture);
    free(input);
    capture = NULL;
    input = NULL;
    if (detector_data) {
        detector->destroy(detector_data);
        detector_data = NULL;
    }
    esp_srmodel_deinit(model_list);
    model_list = NULL;
    return err;
}

void wr_wakenet_pause(void)
{
    paused = true;
}

esp_err_t wr_wakenet_poll(wr_audio_t *audio, bool *matched)
{
    if (!audio || !matched) {
        return ESP_ERR_INVALID_ARG;
    }
    *matched = false;
    if (!detector_data) {
        return ESP_ERR_INVALID_STATE;
    }
    if (paused) {
        detector->clean(detector_data);
        board_resampler_reset(&resampler);
        /* Drain buffered call-era PCM before it can reach the detector. */
        settling_frames = MICROPHONE_RATE / 4;
        paused = false;
    }
    esp_err_t err = wr_audio_read_frames(audio, capture, capture_frames, 80);
    if (err != ESP_OK) {
        wr_wakenet_pause();
        // Clear a latched DMA overflow before the next capture attempt.
        esp_err_t stop_err = wr_audio_pause_rx(audio);
        return stop_err == ESP_OK ? err : stop_err;
    }
    if (settling_frames != 0) {
        settling_frames = settling_frames > capture_frames ? settling_frames - capture_frames : 0;
        return ESP_OK;
    }
    for (size_t i = 0; i < capture_frames; ++i) {
        capture[i] = capture[2 * i];
    }
    if (board_resampler_process(&resampler, capture, capture_frames, input, model_samples) !=
        (int)model_samples) {
        wr_wakenet_pause();
        return ESP_ERR_INVALID_SIZE;
    }
    *matched = detector->detect(detector_data, input) > WAKENET_NO_DETECT;
    if (!stack_reported) {
        stack_reported = true;
        ESP_LOGD("wakenet", "app_main minimum free stack after first detection frame: %u bytes",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    if (*matched) {
        wr_wakenet_pause();
    }
    return ESP_OK;
}

#endif

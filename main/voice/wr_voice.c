#include "wr_voice.h"
#include "wr_frame.h"
#include "audio/wr_opus.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

enum {
    VOICE_NETWORK_LOST = WR_VOICE_ERROR + 1,
    PLAYBACK_DEPTH = 4,
    IO_MS = 250,
    NETWORK_MS = 3000,
    CONNECT_MS = 12000,
    URI_MAX = 512,
    TOKEN_MAX = 384,
};

_Static_assert(WR_FRAME_MAX == WR_OPUS_PACKET_MAX, "WebSocket and codec packet limits must agree");

typedef struct {
    size_t size;
    uint8_t data[WR_FRAME_MAX];
} voice_packet_t;

typedef struct {
    wr_audio_t *audio;
    wr_opus_t *codec;
    QueueHandle_t incoming;
    SemaphoreHandle_t playback_done;
    atomic_bool connected;
    atomic_bool peer_closed;
    atomic_bool abort_playback;
    atomic_bool drain_playback;
    atomic_bool failed;
    wr_frame_t frame;
    voice_packet_t incoming_packet;
    voice_packet_t outgoing_packet;
    voice_packet_t playback_packet;
    int16_t stereo[2 * WR_AUDIO_FRAMES];
    int16_t decoded[WR_AUDIO_FRAMES];
    char uri[URI_MAX + 1];
    char headers[sizeof("Authorization: Bearer \r\n") + TOKEN_MAX];
} voice_call_t;

static const char *const TAG = "wr_voice";
static atomic_int state = WR_VOICE_IDLE;
static _Atomic(TaskHandle_t) ws_task;
static unsigned ws_connect_depth;

int __real_esp_transport_connect(esp_transport_handle_t transport, const char *host, int port, int timeout_ms);

/* The managed client follows redirects with its original headers. Accept only
 * upgrade 101 for this task's outer WebSocket transport, not its inner TLS call. */
int __wrap_esp_transport_connect(esp_transport_handle_t transport, const char *host, int port, int timeout_ms)
{
    if (xTaskGetCurrentTaskHandle() != atomic_load(&ws_task)) {
        return __real_esp_transport_connect(transport, host, port, timeout_ms);
    }
    bool outer = ws_connect_depth++ == 0;
    int result = __real_esp_transport_connect(transport, host, port, timeout_ms);
    --ws_connect_depth;
    if (outer && result >= 0 && esp_transport_ws_get_upgrade_request_status(transport) != 101) {
        errno = EPROTO;
        return -1;
    }
    return result;
}

static void report_failure(voice_call_t *call, const char *reason)
{
    if (!atomic_exchange(&call->failed, true)) {
        ESP_LOGE(TAG, "%s", reason);
    }
}

wr_voice_state_t wr_voice_state(void)
{
    int current = atomic_load(&state);
    return current == VOICE_NETWORK_LOST ? WR_VOICE_STOPPING : (wr_voice_state_t)current;
}

static void request_stop(int destination)
{
    int current = atomic_load(&state);
    while (current == WR_VOICE_CONNECTING || current == WR_VOICE_ACTIVE) {
        if (atomic_compare_exchange_weak(&state, &current, destination)) {
            return;
        }
    }
}

void wr_voice_stop(void)
{
    request_stop(WR_VOICE_STOPPING);
}

void wr_voice_network_lost(void)
{
    request_stop(VOICE_NETWORK_LOST);
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    (void)base;
    voice_call_t *call = arg;
    const esp_websocket_event_data_t *part = data;
    switch (event) {
    case WEBSOCKET_EVENT_BEGIN:
        ws_connect_depth = 0;
        atomic_store(&ws_task, xTaskGetCurrentTaskHandle());
        break;
    case WEBSOCKET_EVENT_FINISH:
        atomic_store(&ws_task, NULL);
        break;
    case WEBSOCKET_EVENT_CONNECTED:
        atomic_store(&call->connected, true);
        break;
    case WEBSOCKET_EVENT_ERROR:
    case WEBSOCKET_EVENT_DISCONNECTED:
        if (wr_voice_state() != WR_VOICE_STOPPING && !atomic_load(&call->peer_closed)) {
            report_failure(call, "WebSocket transport failed");
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (!part || wr_voice_state() == WR_VOICE_STOPPING || atomic_load(&call->failed) ||
            atomic_load(&call->peer_closed)) {
            break;
        }
        if (part->op_code == 8) {
            if (!part->fin || call->frame.reading || call->frame.continuing || part->payload_len > 125) {
                report_failure(call, "Gateway ended an incomplete WebSocket message");
            } else if (part->payload_len == 0 || (part->payload_len >= 2 && part->payload_offset == 0 &&
                part->data_len >= 2 && part->data_ptr && (uint8_t)part->data_ptr[0] == 0x03 &&
                (uint8_t)part->data_ptr[1] == 0xe8)) {
                atomic_store(&call->peer_closed, true);
            } else {
                report_failure(call, "Gateway closed with an unsuccessful status");
            }
            break;
        }
        if (part->payload_len < 0 || part->payload_offset < 0 || part->data_len < 0) {
            report_failure(call, "Invalid WebSocket payload length");
            break;
        }
        switch (wr_frame_feed(&call->frame, part->op_code, part->fin, part->payload_len,
                              part->payload_offset, (const uint8_t *)part->data_ptr, part->data_len)) {
        case WR_FRAME_READY:
            call->incoming_packet.size = call->frame.size;
            memcpy(call->incoming_packet.data, call->frame.data, call->frame.size);
            if (xQueueSend(call->incoming, &call->incoming_packet, 0) != pdTRUE) {
                report_failure(call, "Audio playback queue overflow");
            }
            break;
        case WR_FRAME_INVALID:
            report_failure(call, "Invalid Opus WebSocket message");
            break;
        default:
            break;
        }
        break;
    case WEBSOCKET_EVENT_CLOSED:
        if (wr_voice_state() != WR_VOICE_STOPPING && !atomic_load(&call->peer_closed)) {
            report_failure(call, "WebSocket closed without a normal close frame");
        }
        break;
    default:
        break;
    }
}

static void playback_task(void *arg)
{
    voice_call_t *call = arg;
    while (!atomic_load(&call->abort_playback) && !atomic_load(&call->failed) &&
           wr_voice_state() != WR_VOICE_STOPPING) {
        if (xQueueReceive(call->incoming, &call->playback_packet, pdMS_TO_TICKS(40)) != pdTRUE) {
            if (atomic_load(&call->drain_playback) && uxQueueMessagesWaiting(call->incoming) == 0) {
                break;
            }
            continue;
        }
        int frames = wr_opus_decode(call->codec, call->playback_packet.data,
                                    call->playback_packet.size, call->decoded);
        if (frames != WR_AUDIO_FRAMES) {
            report_failure(call, "Downlink is not a valid 60 ms Opus packet");
            break;
        }
        if (wr_audio_play(call->audio, call->decoded, (size_t)frames, IO_MS) != ESP_OK) {
            report_failure(call, "Speaker write failed");
            break;
        }
    }
    if (atomic_load(&call->drain_playback) && !atomic_load(&call->failed) &&
        wr_voice_state() != WR_VOICE_STOPPING && wr_audio_drain(call->audio, IO_MS) != ESP_OK) {
        report_failure(call, "Could not drain speaker output");
    }
    if (wr_audio_mute(call->audio) != ESP_OK) {
        report_failure(call, "Could not mute speaker output");
    }
    xSemaphoreGive(call->playback_done);
    vTaskDelete(NULL);
}

static void capture_loop(voice_call_t *call, esp_websocket_client_handle_t ws)
{
    if (wr_audio_pause_rx(call->audio) != ESP_OK) {
        report_failure(call, "Could not reset microphone capture");
        return;
    }
    while (wr_voice_state() == WR_VOICE_ACTIVE && !atomic_load(&call->failed) &&
           !atomic_load(&call->peer_closed)) {
        if (wr_audio_read(call->audio, call->stereo, IO_MS) != ESP_OK) {
            report_failure(call, "Microphone read failed");
            break;
        }
        if (wr_voice_state() != WR_VOICE_ACTIVE || atomic_load(&call->failed) ||
            atomic_load(&call->peer_closed)) {
            break;
        }
        for (size_t i = 0; i < WR_AUDIO_FRAMES; ++i) {
            call->stereo[i] = call->stereo[2 * i];
        }
        int bytes = wr_opus_encode(call->codec, call->stereo, call->outgoing_packet.data,
                                   sizeof(call->outgoing_packet.data));
        if (bytes <= 0 || bytes > WR_FRAME_MAX) {
            report_failure(call, "Opus encoder failed");
            break;
        }
        int sent = esp_websocket_client_send_bin(ws, (const char *)call->outgoing_packet.data,
                                                bytes, pdMS_TO_TICKS(NETWORK_MS));
        if (sent != bytes && !atomic_load(&call->peer_closed) && wr_voice_state() == WR_VOICE_ACTIVE) {
            report_failure(call, "WebSocket audio send failed");
            break;
        }
    }
    if (wr_audio_pause_rx(call->audio) != ESP_OK) {
        report_failure(call, "Could not release microphone capture");
    }
}

static void call_task(void *arg)
{
    voice_call_t *call = arg;
    esp_websocket_client_handle_t ws = NULL;
    bool playback_started = false;
    bool ws_started = false;
    const esp_websocket_client_config_t config = {
        .uri = call->uri,
        .headers = call->headers[0] ? call->headers : NULL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
        .network_timeout_ms = NETWORK_MS,
        .buffer_size = 1024,
    };

    /* IDF's transport can print the entire upgrade on a write failure. */
    if (call->headers[0]) {
        esp_log_level_set("transport_ws", ESP_LOG_NONE);
        if (esp_log_level_get("transport_ws") != ESP_LOG_NONE) {
            report_failure(call, "Cannot protect authorization header without per-tag log control");
            goto done;
        }
    }
    call->incoming = xQueueCreate(PLAYBACK_DEPTH, sizeof(voice_packet_t));
    call->playback_done = xSemaphoreCreateBinary();
    if (!call->incoming || !call->playback_done || wr_opus_open(&call->codec) != 0 || !call->codec) {
        report_failure(call, "Could not allocate call audio resources");
        goto done;
    }
    ws = esp_websocket_client_init(&config);
    if (!ws || esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, on_ws_event, call) != ESP_OK) {
        report_failure(call, "Could not initialize WebSocket");
        goto done;
    }
    if (wr_voice_state() == WR_VOICE_STOPPING) {
        goto done;
    }
    if (esp_websocket_client_start(ws) != ESP_OK) {
        report_failure(call, "Could not start WebSocket");
        goto done;
    }
    ws_started = true;
    TickType_t start = xTaskGetTickCount();
    while (!atomic_load(&call->connected) && !atomic_load(&call->failed) &&
           wr_voice_state() == WR_VOICE_CONNECTING) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(CONNECT_MS)) {
            report_failure(call, "WebSocket connection timed out");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    int connecting = WR_VOICE_CONNECTING;
    if (atomic_load(&call->failed) ||
        !atomic_compare_exchange_strong(&state, &connecting, WR_VOICE_ACTIVE)) {
        goto done;
    }
    playback_started = xTaskCreate(playback_task, "wr_play", 8192, call, 6, NULL) == pdPASS;
    if (!playback_started) {
        report_failure(call, "Could not start speaker task");
        goto done;
    }
    capture_loop(call, ws);

done:
    if (atomic_load(&state) == VOICE_NETWORK_LOST) {
        report_failure(call, "Network became unavailable during the call");
    }
    if (atomic_load(&call->peer_closed) && !atomic_load(&call->failed)) {
        atomic_store(&call->drain_playback, true);
    } else {
        atomic_store(&call->abort_playback, true);
    }
    if (ws_started && atomic_load(&state) == WR_VOICE_STOPPING && esp_websocket_client_is_connected(ws)) {
        if (esp_websocket_client_close(ws, pdMS_TO_TICKS(NETWORK_MS)) != ESP_OK) {
            report_failure(call, "WebSocket close handshake failed");
        }
    }
    if (ws) {
        esp_websocket_client_destroy(ws);
    }
    if (playback_started) {
        xSemaphoreTake(call->playback_done, portMAX_DELAY);
    }
    if (call->codec) {
        wr_opus_close(call->codec);
    }
    if (call->incoming) {
        vQueueDelete(call->incoming);
    }
    if (call->playback_done) {
        vSemaphoreDelete(call->playback_done);
    }
    bool failed = atomic_load(&call->failed);
    free(call);
    int ending = atomic_load(&state);
    int terminal;
    do {
        terminal = failed || ending == VOICE_NETWORK_LOST ? WR_VOICE_ERROR : WR_VOICE_IDLE;
    } while (!atomic_compare_exchange_weak(&state, &ending, terminal));
    vTaskDelete(NULL);
}

static bool uri_valid(const char *uri)
{
    if (!uri || strlen(uri) > URI_MAX || strncmp(uri, "wss://", 6) != 0 ||
        !uri[6] || uri[6] == '/' || uri[6] == '?') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)uri + 6; *p; ++p) {
        if (*p <= ' ' || *p >= 127 || *p == '@' || *p == '#' || *p == '?' || *p == '\\') {
            return false;
        }
    }
    return true;
}

static bool token_valid(const char *token)
{
    if (!token) {
        return true;
    }
    if (strlen(token) > TOKEN_MAX) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)token; *p; ++p) {
        if (*p <= ' ' || *p >= 127) {
            return false;
        }
    }
    return true;
}

esp_err_t wr_voice_start(wr_audio_t *audio, const char *uri, const char *token)
{
    if (!audio || !uri_valid(uri) || !token_valid(token)) {
        return ESP_ERR_INVALID_ARG;
    }
    int current = atomic_load(&state);
    do {
        if (current != WR_VOICE_IDLE && current != WR_VOICE_ERROR) {
            return ESP_ERR_INVALID_STATE;
        }
    } while (!atomic_compare_exchange_weak(&state, &current, WR_VOICE_CONNECTING));

    voice_call_t *call = calloc(1, sizeof(*call));
    if (!call) {
        atomic_store(&state, WR_VOICE_ERROR);
        return ESP_ERR_NO_MEM;
    }
    call->audio = audio;
    atomic_init(&call->connected, false);
    atomic_init(&call->peer_closed, false);
    atomic_init(&call->abort_playback, false);
    atomic_init(&call->drain_playback, false);
    atomic_init(&call->failed, false);
    memcpy(call->uri, uri, strlen(uri) + 1);
    if (token && token[0]) {
        snprintf(call->headers, sizeof(call->headers), "Authorization: Bearer %s\r\n", token);
    }
    if (xTaskCreate(call_task, "wr_call", 8192, call, 5, NULL) != pdPASS) {
        free(call);
        atomic_store(&state, WR_VOICE_ERROR);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

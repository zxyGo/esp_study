#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "app_config.h"
#include "app_lcd.h"
#include "app_stt.h"

static const char *TAG = "APP_STT";

static esp_websocket_client_handle_t s_ws;
static volatile bool s_ws_ok;
static char s_last_text[128];

static void stt_send_session_update(void)
{
    const char *session_json =
        "{"
            "\"type\":\"session.update\","
            "\"session\":{"
                "\"type\":\"transcription\","
                "\"audio\":{"
                    "\"input\":{"
                        "\"format\":{\"type\":\"audio/pcm\",\"rate\":24000},"
                        "\"transcription\":{"
                            "\"model\":\"gpt-realtime-whisper\","
                            "\"language\":\"zh\","
                            "\"delay\":\"low\""
                        "}"
                    "}"
                "}"
            "}"
        "}";

    esp_websocket_client_send_text(s_ws, session_json, strlen(session_json),
                                   pdMS_TO_TICKS(1000));
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_ws_ok = true;
        ESP_LOGI(TAG, "OpenAI Realtime WebSocket 已连接");
        app_lcd_status_line(80, "服务:已连接", C_GREEN);
        stt_send_session_update();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        s_ws_ok = false;
        ESP_LOGW(TAG, "OpenAI Realtime WebSocket 已断开");
        app_lcd_status_line(80, "服务:断开", C_RED);
        break;

    case WEBSOCKET_EVENT_DATA: {
        if (data->data_len <= 0) break;

        char *json = malloc(data->data_len + 1);
        if (!json) break;
        memcpy(json, data->data_ptr, data->data_len);
        json[data->data_len] = '\0';

        cJSON *root = cJSON_Parse(json);
        if (root) {
            cJSON *type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring,
                           "conversation.item.input_audio_transcription.delta") == 0) {
                    cJSON *delta = cJSON_GetObjectItem(root, "delta");
                    if (cJSON_IsString(delta)) {
                        ESP_LOGI(TAG, "实时片段: %s", delta->valuestring);
                    }
                } else if (strcmp(type->valuestring,
                                  "conversation.item.input_audio_transcription.completed") == 0) {
                    cJSON *transcript = cJSON_GetObjectItem(root, "transcript");
                    if (cJSON_IsString(transcript)) {
                        snprintf(s_last_text, sizeof(s_last_text), "%s", transcript->valuestring);
                        ESP_LOGI(TAG, "最终文字: %s", s_last_text);
                        app_lcd_show_transcript_hint(s_last_text);
                    }
                } else if (strcmp(type->valuestring, "error") == 0) {
                    ESP_LOGW(TAG, "OpenAI 返回错误: %s", json);
                    app_lcd_status_line(80, "服务:错误", C_RED);
                }
            }
            cJSON_Delete(root);
        }
        free(json);
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        s_ws_ok = false;
        ESP_LOGW(TAG, "WebSocket 发生错误");
        app_lcd_status_line(80, "服务:错误", C_RED);
        break;

    default:
        break;
    }
}

void app_stt_start(void)
{
    if (strncmp(OPENAI_API_KEY, "sk-", 3) != 0) {
        ESP_LOGW(TAG, "请先在 app_config.h 里填写 OPENAI_API_KEY");
        app_lcd_status_line(80, "请填密钥", C_RED);
        return;
    }

    static char headers[256];
    snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", OPENAI_API_KEY);

    esp_websocket_client_config_t websocket_cfg = {
        .uri = OPENAI_REALTIME_URL,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .network_timeout_ms = 10000,
    };

    s_ws = esp_websocket_client_init(&websocket_cfg);
    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                                                  websocket_event_handler, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(s_ws));
}

bool app_stt_is_connected(void)
{
    return s_ws_ok;
}

void app_stt_send_audio_chunk(const int16_t *pcm, int frames)
{
    if (!s_ws_ok || !s_ws || frames <= 0) return;

    const size_t pcm_bytes = frames * sizeof(int16_t);
    unsigned char b64[STT_CHUNK_FRAMES * 2 * 4 / 3 + 8];
    size_t b64_len = 0;

    int ret = mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
                                    (const unsigned char *)pcm, pcm_bytes);
    if (ret != 0) {
        ESP_LOGW(TAG, "base64 编码失败: %d", ret);
        return;
    }

    char json[1600];
    int json_len = snprintf(json, sizeof(json),
                            "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%.*s\"}",
                            (int)b64_len, (char *)b64);
    if (json_len <= 0 || json_len >= (int)sizeof(json)) {
        ESP_LOGW(TAG, "音频 JSON 太长，已跳过本包");
        return;
    }

    esp_websocket_client_send_text(s_ws, json, json_len, pdMS_TO_TICKS(1000));
}

void app_stt_commit_audio(void)
{
    if (!s_ws_ok || !s_ws) return;

    const char *commit_json = "{\"type\":\"input_audio_buffer.commit\"}";
    esp_websocket_client_send_text(s_ws, commit_json, strlen(commit_json),
                                   pdMS_TO_TICKS(1000));
}

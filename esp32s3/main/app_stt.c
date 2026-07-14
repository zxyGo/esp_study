#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "app_config.h"
#include "app_lcd.h"
#include "app_llm.h"
#include "app_stt.h"

/*
 * FunASR WebSocket 协议（2pass 在线流式模式）
 *
 * 1. 连接 ws://server:10095（无 TLS，无鉴权）
 * 2. 连接成功后发送 JSON 握手（is_speaking:true）
 * 3. 持续发送原始二进制 PCM16 数据帧
 * 4. 服务端靠 VAD 自动检测句子边界并回传结果：
 *    - mode:"2pass-online"  → 实时片段（可能不完整）
 *    - mode:"2pass-offline" → VAD 断句后的完整句子
 * 5. 会话结束时发送 is_speaking:false（可选，连续流式可不发）
 */

static const char *TAG = "APP_STT";

static esp_websocket_client_handle_t s_ws;
static volatile bool s_ws_ok;
static char s_last_text[256];

/* FunASR 握手消息：声明音频格式与流式模式 */
static const char *FUNASR_START_MSG =
    "{"
        "\"mode\":\"2pass\","
        "\"wav_name\":\"stream\","
        "\"is_speaking\":true,"
        "\"wav_format\":\"pcm\","
        "\"chunk_size\":[5,10,5],"  /* 单位 10 ms：前缓冲 50 ms / 当前 100 ms / 后缓冲 50 ms */
        "\"audio_fs\":16000,"
        "\"hotwords\":\"\""
    "}";

static void stt_send_start(void)
{
    esp_websocket_client_send_text(s_ws, FUNASR_START_MSG,
                                   strlen(FUNASR_START_MSG),
                                   pdMS_TO_TICKS(1000));
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_ws_ok = true;
        ESP_LOGI(TAG, "FunASR WebSocket 已连接");
        app_lcd_status_line(80, "服务:已连接", C_GREEN);
        stt_send_start();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        s_ws_ok = false;
        ESP_LOGW(TAG, "FunASR WebSocket 已断开，等待自动重连...");
        app_lcd_status_line(80, "服务:断开", C_RED);
        break;

    case WEBSOCKET_EVENT_DATA: {
        if (data->data_len <= 0 || data->op_code != 0x1 /* text frame */) break;

        char *json = malloc(data->data_len + 1);
        if (!json) break;
        memcpy(json, data->data_ptr, data->data_len);
        json[data->data_len] = '\0';

        cJSON *root = cJSON_Parse(json);
        if (root) {
            cJSON *mode = cJSON_GetObjectItem(root, "mode");
            cJSON *text = cJSON_GetObjectItem(root, "text");

            if (cJSON_IsString(mode) && cJSON_IsString(text) && text->valuestring[0] != '\0') {
                if (strcmp(mode->valuestring, "2pass-online") == 0) {
                    /* 实时流式片段：显示在状态行，字体较小 */
                    ESP_LOGI(TAG, "[在线] %s", text->valuestring);
                    app_lcd_status_line(96, text->valuestring, C_CYAN);

                } else if (strcmp(mode->valuestring, "2pass-offline") == 0) {
                    /* VAD 断句后的完整句子：显示在主区域 */
                    snprintf(s_last_text, sizeof(s_last_text), "%s", text->valuestring);
                    ESP_LOGI(TAG, "[离线] %s", s_last_text);
                    app_lcd_show_transcript_hint(s_last_text);
                    app_lcd_status_line(96, "", C_BLACK); /* 清空在线行 */
                    app_llm_ask(s_last_text);
                }
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "JSON 解析失败: %.60s", json);
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
    esp_websocket_client_config_t ws_cfg = {
        .uri                = FUNASR_WS_URL,
        .buffer_size        = 4096,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 5000,
        /* FunASR 本地服务无需 TLS，不配置 crt_bundle */
    };

    s_ws = esp_websocket_client_init(&ws_cfg);
    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                                                  websocket_event_handler, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(s_ws));
    ESP_LOGI(TAG, "正在连接 FunASR: %s", FUNASR_WS_URL);
    app_lcd_status_line(80, "服务:连接中", C_YELLOW);
}

bool app_stt_is_connected(void)
{
    return s_ws_ok;
}

void app_stt_send_audio_chunk(const int16_t *pcm, int frames)
{
    if (!s_ws_ok || !s_ws || frames <= 0) return;

    /* FunASR 直接接收原始二进制 PCM16，无需 base64 或 JSON 包装 */
    esp_websocket_client_send_bin(s_ws, (const char *)pcm,
                                  frames * sizeof(int16_t),
                                  pdMS_TO_TICKS(200));
}

void app_stt_commit_audio(void)
{
    /*
     * FunASR 2pass 模式下，VAD 会自动检测静音并触发离线模型出最终结果，
     * 无需客户端主动提交。此函数保留接口兼容性，实际为空操作。
     *
     * 若需要强制刷新（如噪声环境下 VAD 不触发），可在此处发送：
     *   {"is_speaking": false}
     * 并在收到最终结果后重新调用 stt_send_start() 开始新会话。
     */
}

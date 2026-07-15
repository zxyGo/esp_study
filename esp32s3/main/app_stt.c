#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "app_config.h"
#include "app_lcd.h"
#include "app_llm.h"
#include "app_speaker.h"
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
static volatile bool s_waiting_for_question;
static TickType_t s_wake_tick;
static TimerHandle_t s_wake_timeout_timer;
static TaskHandle_t s_wake_reply_task;

static void stt_send_start(void)
{
    cJSON *start = cJSON_CreateObject();
    cJSON *chunk_size = cJSON_CreateIntArray((const int[]){5, 10, 5}, 3);
    if (!start || !chunk_size) {
        cJSON_Delete(start);
        cJSON_Delete(chunk_size);
        ESP_LOGE(TAG, "创建 FunASR 握手消息失败");
        return;
    }

    cJSON_AddStringToObject(start, "mode", "2pass");
    cJSON_AddStringToObject(start, "wav_name", "stream");
    cJSON_AddBoolToObject(start, "is_speaking", true);
    cJSON_AddStringToObject(start, "wav_format", "pcm");
    cJSON_AddItemToObject(start, "chunk_size", chunk_size);
    cJSON_AddNumberToObject(start, "audio_fs", STT_SAMPLE_RATE);
    /* FunASR 协议要求 hotwords 的值是一个 JSON 对象序列化后的字符串。 */
    cJSON_AddStringToObject(start, "hotwords", WAKE_HOTWORDS_JSON);

    char *message = cJSON_PrintUnformatted(start);
    if (message) {
        esp_websocket_client_send_text(s_ws, message, strlen(message),
                                       pdMS_TO_TICKS(1000));
        cJSON_free(message);
    }
    cJSON_Delete(start);
}

static const char *skip_separators(const char *text)
{
    static const char *const utf8_separators[] = {
        "，", "。", "！", "？", "、", "：", "；", "“", "”", "‘", "’"
    };

    while (text && *text) {
        unsigned char ch = (unsigned char)*text;
        if (ch < 0x80 && strchr(" \t\r\n,.;:!?-_\"'", ch)) {
            ++text;
            continue;
        }

        bool matched = false;
        for (size_t i = 0; i < sizeof(utf8_separators) / sizeof(utf8_separators[0]); ++i) {
            size_t len = strlen(utf8_separators[i]);
            if (strncmp(text, utf8_separators[i], len) == 0) {
                text += len;
                matched = true;
                break;
            }
        }
        if (!matched) break;
    }
    return text;
}

typedef struct {
    uint32_t codepoint;
    const char *end;
} wake_char_t;

/* 解码一个 UTF-8 字符。遇到非法序列时按单字节处理，避免越界。 */
static bool read_utf8_char(const char **cursor, uint32_t *codepoint, const char **end)
{
    const unsigned char *s = (const unsigned char *)*cursor;
    if (!s || !s[0]) return false;

    size_t length = 1;
    uint32_t value = s[0];
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        length = 2;
        value = ((uint32_t)(s[0] & 0x1F) << 6) |
                (uint32_t)(s[1] & 0x3F);
    } else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2] &&
               (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        length = 3;
        value = ((uint32_t)(s[0] & 0x0F) << 12) |
                ((uint32_t)(s[1] & 0x3F) << 6) |
                (uint32_t)(s[2] & 0x3F);
    } else if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3] &&
               (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
               (s[3] & 0xC0) == 0x80) {
        length = 4;
        value = ((uint32_t)(s[0] & 0x07) << 18) |
                ((uint32_t)(s[1] & 0x3F) << 12) |
                ((uint32_t)(s[2] & 0x3F) << 6) |
                (uint32_t)(s[3] & 0x3F);
    }

    *cursor += length;
    *codepoint = value;
    *end = *cursor;
    return true;
}

static unsigned wake_edit_distance(const uint32_t *candidate, size_t candidate_len)
{
    /* “你好禹神”的 Unicode 码点；按字符计算，不能直接对 UTF-8 字节计算距离。 */
    static const uint32_t target[] = {0x4F60, 0x597D, 0x79B9, 0x795E};
    enum { TARGET_LEN = sizeof(target) / sizeof(target[0]) };
    unsigned previous[TARGET_LEN + WAKE_FUZZY_MAX_EDITS + 1];
    unsigned current[TARGET_LEN + WAKE_FUZZY_MAX_EDITS + 1];

    for (size_t j = 0; j <= candidate_len; ++j) previous[j] = (unsigned)j;
    for (size_t i = 1; i <= TARGET_LEN; ++i) {
        current[0] = (unsigned)i;
        for (size_t j = 1; j <= candidate_len; ++j) {
            unsigned deletion = previous[j] + 1;
            unsigned insertion = current[j - 1] + 1;
            unsigned substitution = previous[j - 1] +
                                    (target[i - 1] == candidate[j - 1] ? 0 : 1);
            unsigned best = deletion < insertion ? deletion : insertion;
            current[j] = best < substitution ? best : substitution;
        }
        memcpy(previous, current, (candidate_len + 1) * sizeof(previous[0]));
    }
    return previous[candidate_len];
}

static bool codepoint_is_one_of(uint32_t codepoint, const uint32_t *values,
                                size_t value_count)
{
    for (size_t i = 0; i < value_count; ++i) {
        if (codepoint == values[i]) return true;
    }
    return false;
}

/* “禹神”很容易被 ASR 写成“余生”“雨声”等两个字都不同的近音词。
 * 仅在前两个字仍为“你好”时接受这组白名单，避免把通用阈值放宽到 2。 */
static bool is_wake_homophone(const uint32_t *candidate, size_t candidate_len)
{
    static const uint32_t yu_variants[] = {
        0x79B9, /* 禹 */ 0x96E8, /* 雨 */ 0x5B87, /* 宇 */
        0x8BED, /* 语 */ 0x7FBD, /* 羽 */ 0x4F59, /* 余 */
        0x4E8E, /* 于 */ 0x9C7C, /* 鱼 */ 0x7389, /* 玉 */
    };
    static const uint32_t shen_variants[] = {
        0x795E, /* 神 */ 0x6DF1, /* 深 */ 0x8EAB, /* 身 */
        0x7533, /* 申 */ 0x58F0, /* 声 */ 0x751F, /* 生 */
    };

    return candidate_len == 4 &&
           candidate[0] == 0x4F60 && candidate[1] == 0x597D &&
           codepoint_is_one_of(candidate[2], yu_variants,
                               sizeof(yu_variants) / sizeof(yu_variants[0])) &&
           codepoint_is_one_of(candidate[3], shen_variants,
                               sizeof(shen_variants) / sizeof(shen_variants[0]));
}

/* 仅在句首做模糊匹配，避免正文中偶然提到唤醒词而误唤醒。允许一个汉字
 * 替换、缺失或冗余。返回唤醒词后的正文；空字符串表示本句只有唤醒词。 */
static const char *match_wake_prefix(const char *text, unsigned *matched_edits)
{
    enum {
        TARGET_LEN = 4,
        MIN_CANDIDATE_LEN = TARGET_LEN - WAKE_FUZZY_MAX_EDITS,
        MAX_CANDIDATE_LEN = TARGET_LEN + WAKE_FUZZY_MAX_EDITS,
    };
    wake_char_t chars[MAX_CANDIDATE_LEN];
    uint32_t candidate[MAX_CANDIDATE_LEN];
    size_t char_count = 0;
    const char *cursor = text;

    while (char_count < MAX_CANDIDATE_LEN) {
        cursor = skip_separators(cursor);
        if (!read_utf8_char(&cursor, &chars[char_count].codepoint,
                            &chars[char_count].end)) {
            break;
        }
        candidate[char_count] = chars[char_count].codepoint;
        ++char_count;
    }

    unsigned best_distance = WAKE_FUZZY_MAX_EDITS + 1;
    size_t best_length = 0;
    for (size_t length = MIN_CANDIDATE_LEN;
         length <= char_count && length <= MAX_CANDIDATE_LEN; ++length) {
        unsigned distance = wake_edit_distance(candidate, length);
        if (is_wake_homophone(candidate, length) && distance > 1) {
            distance = 1;
        }
        /* 距离相同时保留较短前缀，避免把问题的第一个字误当成唤醒词。 */
        if (distance < best_distance) {
            best_distance = distance;
            best_length = length;
        }
    }

    if (best_length == 0 || best_distance > WAKE_FUZZY_MAX_EDITS) return NULL;
    if (matched_edits) *matched_edits = best_distance;
    return skip_separators(chars[best_length - 1].end);
}

static void wake_timeout_callback(TimerHandle_t timer)
{
    (void)timer;
    if (!s_waiting_for_question) return;

    s_waiting_for_question = false;
    ESP_LOGI(TAG, "60 秒内没有收到问题，恢复待唤醒状态");
    app_lcd_status_line(188, "请说：你好，禹神", C_ORANGE);
}

static void arm_wake_timeout(void)
{
    s_wake_tick = xTaskGetTickCount();
    if (s_wake_timeout_timer) {
        xTimerReset(s_wake_timeout_timer, 0);
    }
}

static void cancel_wake_timeout(void)
{
    if (s_wake_timeout_timer) {
        xTimerStop(s_wake_timeout_timer, 0);
    }
}

static void wake_reply_task_main(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_waiting_for_question) continue;

        esp_err_t err = app_speaker_play_text(WAKE_REPLY_TEXT);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "唤醒回复播放失败: %s", esp_err_to_name(err));
        }

        /* 播报期间暂停上传麦克风，因此从播报及回声保护结束后计时一分钟。 */
        if (s_waiting_for_question) {
            arm_wake_timeout();
        }
    }
}

static void init_wake_control(void)
{
    if (!s_wake_timeout_timer) {
        s_wake_timeout_timer = xTimerCreate(
            "wake_timeout", pdMS_TO_TICKS(WAKE_LISTEN_TIMEOUT_MS),
            pdFALSE, NULL, wake_timeout_callback);
        if (!s_wake_timeout_timer) {
            ESP_LOGE(TAG, "创建唤醒超时定时器失败");
        }
    }

    if (!s_wake_reply_task &&
        xTaskCreate(wake_reply_task_main, "wake_reply", 6144, NULL, 4,
                    &s_wake_reply_task) != pdPASS) {
        s_wake_reply_task = NULL;
        ESP_LOGE(TAG, "创建唤醒回复任务失败");
    }
}

static bool wake_window_is_open(void)
{
    if (!s_waiting_for_question) return false;

    TickType_t elapsed = xTaskGetTickCount() - s_wake_tick;
    if (elapsed >= pdMS_TO_TICKS(WAKE_LISTEN_TIMEOUT_MS)) {
        s_waiting_for_question = false;
        cancel_wake_timeout();
        ESP_LOGI(TAG, "60 秒内没有收到问题，恢复待唤醒状态");
        app_lcd_status_line(188, "请说：你好，禹神", C_ORANGE);
        return false;
    }
    return true;
}

static void handle_final_text(const char *text)
{
    unsigned matched_edits = 0;
    const char *command = match_wake_prefix(text, &matched_edits);
    if (command) {
        ESP_LOGI(TAG, "唤醒词模糊匹配成功: 编辑距离=%u, 识别文本=%s",
                 matched_edits, text);
        if (*command == '\0') {
            cancel_wake_timeout();
            s_waiting_for_question = true;
            s_wake_tick = xTaskGetTickCount();
            ESP_LOGI(TAG, "已被“%s”唤醒，等待提问", WAKE_WORD_DISPLAY);
            app_lcd_show_wake_hint();
            if (s_wake_reply_task) {
                xTaskNotifyGive(s_wake_reply_task);
            } else {
                /* 即使回复任务创建失败，也仍保证一分钟后退出唤醒状态。 */
                arm_wake_timeout();
            }
            return;
        }

        /* 唤醒词和问题在同一句时，去掉唤醒词，只把实际问题发给大模型。 */
        s_waiting_for_question = false;
        cancel_wake_timeout();
        snprintf(s_last_text, sizeof(s_last_text), "%s", command);
    } else if (wake_window_is_open()) {
        s_waiting_for_question = false;
        cancel_wake_timeout();
        snprintf(s_last_text, sizeof(s_last_text), "%s", text);
    } else {
        ESP_LOGI(TAG, "[未唤醒] 忽略: %s", text);
        return;
    }

    ESP_LOGI(TAG, "[提问] %s", s_last_text);
    app_lcd_show_transcript_hint(s_last_text);
    app_lcd_status_line(96, "", C_BLACK);
    app_llm_ask(s_last_text);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_ws_ok = true;
        s_waiting_for_question = false;
        cancel_wake_timeout();
        ESP_LOGI(TAG, "FunASR WebSocket 已连接");
        app_lcd_status_line(80, "服务:已连接", C_GREEN);
        stt_send_start();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        s_ws_ok = false;
        s_waiting_for_question = false;
        cancel_wake_timeout();
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
                    ESP_LOGI(TAG, "[离线] %s", text->valuestring);
                    handle_final_text(text->valuestring);
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
        s_waiting_for_question = false;
        cancel_wake_timeout();
        ESP_LOGW(TAG, "WebSocket 发生错误");
        app_lcd_status_line(80, "服务:错误", C_RED);
        break;

    default:
        break;
    }
}

void app_stt_start(void)
{
    init_wake_control();

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

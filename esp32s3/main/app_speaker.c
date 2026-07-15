#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_speaker.h"

static const char *TAG = "APP_SPEAKER";

typedef struct {
    bool got_audio;
    bool has_pending_byte;
    bool format_error;
    uint8_t pending_byte;
    esp_err_t write_error;
    uint8_t *audio;
    size_t audio_length;
    size_t audio_capacity;
} speaker_response_t;

static i2s_chan_handle_t s_tx;
static volatile bool s_playing;
static SemaphoreHandle_t s_play_mutex;
static int16_t s_stereo_buffer[512 * 2];

static esp_err_t write_mono_pcm_as_stereo(speaker_response_t *response,
                                          const uint8_t *data, size_t length)
{
    /* MAX98357A 可通过 SD/MODE 选择左右声道；复制到两边后无需依赖模块焊盘配置。 */
    size_t frames = 0;
    size_t offset = 0;

    while (offset < length || response->has_pending_byte) {
        uint8_t low;
        uint8_t high;

        if (response->has_pending_byte) {
            if (offset >= length) break;
            low = response->pending_byte;
            high = data[offset++];
            response->has_pending_byte = false;
        } else {
            low = data[offset++];
            if (offset >= length) {
                response->pending_byte = low;
                response->has_pending_byte = true;
                break;
            }
            high = data[offset++];
        }

        int16_t sample = (int16_t)((uint16_t)low | ((uint16_t)high << 8));
        s_stereo_buffer[frames * 2] = sample;
        s_stereo_buffer[frames * 2 + 1] = sample;
        frames++;

        if (frames == 512) {
            size_t bytes_written = 0;
            esp_err_t err = i2s_channel_write(s_tx, s_stereo_buffer,
                                              sizeof(s_stereo_buffer),
                                              &bytes_written, 1000);
            if (err != ESP_OK || bytes_written != sizeof(s_stereo_buffer)) {
                return err == ESP_OK ? ESP_FAIL : err;
            }
            frames = 0;
        }
    }

    if (frames > 0) {
        size_t bytes = frames * 2 * sizeof(int16_t);
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_stereo_buffer, bytes,
                                          &bytes_written, 1000);
        if (err != ESP_OK || bytes_written != bytes) {
            return err == ESP_OK ? ESP_FAIL : err;
        }
    }
    return ESP_OK;
}

static esp_err_t speaker_http_event_handler(esp_http_client_event_t *event)
{
    speaker_response_t *response = (speaker_response_t *)event->user_data;
    if (!response) return ESP_OK;

    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
        strcasecmp(event->header_key, "X-Audio-Sample-Rate") == 0) {
        int sample_rate = atoi(event->header_value);
        if (sample_rate != TTS_SAMPLE_RATE) {
            ESP_LOGE(TAG, "TTS 采样率不匹配: 服务端=%d, 固件=%d",
                     sample_rate, TTS_SAMPLE_RATE);
            response->format_error = true;
        }
    }

    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    if (esp_http_client_get_status_code(event->client) != 200 || response->format_error ||
        response->write_error != ESP_OK) {
        return ESP_OK;
    }

    size_t incoming = (size_t)event->data_len;
    if (incoming > TTS_MAX_AUDIO_BYTES - response->audio_length) {
        response->write_error = ESP_ERR_INVALID_SIZE;
        return response->write_error;
    }

    size_t required = response->audio_length + incoming;
    if (required > response->audio_capacity) {
        size_t capacity = response->audio_capacity ? response->audio_capacity : 64 * 1024;
        while (capacity < required && capacity < TTS_MAX_AUDIO_BYTES) {
            capacity *= 2;
        }
        if (capacity > TTS_MAX_AUDIO_BYTES) capacity = TTS_MAX_AUDIO_BYTES;

        uint8_t *audio = heap_caps_realloc(
            response->audio, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!audio) {
            response->write_error = ESP_ERR_NO_MEM;
            return response->write_error;
        }
        response->audio = audio;
        response->audio_capacity = capacity;
    }

    memcpy(response->audio + response->audio_length, event->data, incoming);
    response->audio_length += incoming;
    response->got_audio = true;
    return ESP_OK;
}

static esp_err_t play_buffered_pcm(speaker_response_t *response)
{
    if (!response->audio || response->audio_length == 0 || response->audio_length % 2) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    int32_t peak = 0;
    const int16_t *samples = (const int16_t *)response->audio;
    size_t sample_count = response->audio_length / sizeof(int16_t);
    for (size_t i = 0; i < sample_count; ++i) {
        int32_t magnitude = samples[i] == INT16_MIN ? 32768 : abs(samples[i]);
        if (magnitude > peak) peak = magnitude;
    }

    ESP_LOGI(TAG, "已接收 TTS PCM: %u bytes, peak=%ld (%ld%%), duration=%u ms",
             (unsigned)response->audio_length,
             (long)peak,
             (long)(peak * 100 / 32768),
             (unsigned)(sample_count * 1000 / TTS_SAMPLE_RATE));
    if (peak == 0) return ESP_ERR_INVALID_RESPONSE;

    /* 重新启动 TX，清除上一次播放后的 DMA 欠载状态，再连续写入完整音频。 */
    esp_err_t err = i2s_channel_disable(s_tx);
    if (err != ESP_OK) return err;
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) return err;

    response->has_pending_byte = false;
    return write_mono_pcm_as_stereo(response, response->audio, response->audio_length);
}

void app_speaker_init(void)
{
    if (!s_play_mutex) {
        s_play_mutex = xSemaphoreCreateMutex();
        ESP_ERROR_CHECK(s_play_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    }
    if (s_tx) return;

    /* MAX98357A 的 SD/MODE 内部下拉；悬空时芯片处于关断状态。 */
    ESP_ERROR_CHECK(gpio_set_direction(SPEAKER_SD, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(SPEAKER_SD, 1));
    vTaskDelay(pdMS_TO_TICKS(5));

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(TTS_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPEAKER_BCLK,
            .ws = SPEAKER_WS,
            .dout = SPEAKER_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_LOGI(TAG, "MAX98357A 已初始化: BCLK=%d, WS=%d, DIN=%d, SD=%d, %d Hz",
             SPEAKER_BCLK, SPEAKER_WS, SPEAKER_DOUT, SPEAKER_SD, TTS_SAMPLE_RATE);
}

esp_err_t app_speaker_play_text(const char *text)
{
    if (!s_tx || !text || !text[0]) return ESP_ERR_INVALID_ARG;

    cJSON *request_json = cJSON_CreateObject();
    if (!request_json) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(request_json, "text", text);
    char *post_data = cJSON_PrintUnformatted(request_json);
    cJSON_Delete(request_json);
    if (!post_data) return ESP_ERR_NO_MEM;

    speaker_response_t response = {
        .write_error = ESP_OK,
    };
    esp_http_client_config_t config = {
        .url = TTS_GATEWAY_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = speaker_http_event_handler,
        .user_data = &response,
        .timeout_ms = TTS_REQUEST_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(post_data);
        return ESP_ERR_NO_MEM;
    }

    /* 大模型回答和固定唤醒回复可能来自不同任务，串行访问 I2S 与共享缓冲区。 */
    if (!s_play_mutex || xSemaphoreTake(s_play_mutex, portMAX_DELAY) != pdTRUE) {
        esp_http_client_cleanup(client);
        free(post_data);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    s_playing = true;
    ESP_LOGI(TAG, "正在合成并播放回答");
    esp_err_t result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (result == ESP_OK && status != 200) {
        ESP_LOGW(TAG, "TTS 网关返回 HTTP %d", status);
        result = ESP_FAIL;
    }
    if (result == ESP_OK && response.format_error) result = ESP_ERR_INVALID_RESPONSE;
    if (result == ESP_OK && response.write_error != ESP_OK) result = response.write_error;
    if (result == ESP_OK && !response.got_audio) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    if (result == ESP_OK) result = play_buffered_pcm(&response);

    if (result == ESP_OK) {
        /* 等待最后一个 DMA 块播完及室内回声衰减，期间主循环不会上传麦克风。 */
        vTaskDelay(pdMS_TO_TICKS(SPEAKER_ECHO_GUARD_MS));
        ESP_LOGI(TAG, "语音播放完成");
    } else {
        ESP_LOGW(TAG, "语音播放失败: %s", esp_err_to_name(result));
    }
    s_playing = false;

    esp_http_client_cleanup(client);
    heap_caps_free(response.audio);
    free(post_data);
    xSemaphoreGive(s_play_mutex);
    return result;
}

bool app_speaker_is_playing(void)
{
    return s_playing;
}

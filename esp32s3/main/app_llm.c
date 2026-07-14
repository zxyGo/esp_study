#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_lcd.h"
#include "app_llm.h"
#include "app_speaker.h"

static const char *TAG = "APP_LLM";

typedef struct {
    char text[LLM_PROMPT_MAX_BYTES];
} llm_request_t;

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool overflowed;
} http_response_t;

static QueueHandle_t s_request_queue;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_response_t *response = (http_response_t *)event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !response || event->data_len <= 0) {
        return ESP_OK;
    }

    size_t available = response->capacity - response->length - 1;
    size_t copy_len = (size_t)event->data_len;
    if (copy_len > available) {
        copy_len = available;
        response->overflowed = true;
    }
    if (copy_len > 0) {
        memcpy(response->data + response->length, event->data, copy_len);
        response->length += copy_len;
        response->data[response->length] = '\0';
    }
    return ESP_OK;
}

static esp_err_t request_answer(const char *prompt, char *answer, size_t answer_size)
{
    esp_err_t result = ESP_FAIL;
    char *response_data = calloc(1, LLM_RESPONSE_MAX_BYTES);
    if (!response_data) return ESP_ERR_NO_MEM;
    http_response_t response = {
        .data = response_data,
        .capacity = LLM_RESPONSE_MAX_BYTES,
    };

    cJSON *request_json = cJSON_CreateObject();
    if (!request_json) {
        free(response_data);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(request_json, "text", prompt);
    cJSON_AddStringToObject(request_json, "session_id", LLM_SESSION_ID);
    char *post_data = cJSON_PrintUnformatted(request_json);
    cJSON_Delete(request_json);
    if (!post_data) {
        free(response_data);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = LLM_GATEWAY_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = LLM_REQUEST_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(post_data);
        free(response_data);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "请求网关失败: %s", esp_err_to_name(result));
        goto cleanup;
    }
    if (response.overflowed) {
        ESP_LOGW(TAG, "网关响应超过 %d 字节", LLM_RESPONSE_MAX_BYTES - 1);
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "网关返回 HTTP %d: %.160s", status, response_data);
        result = ESP_FAIL;
        goto cleanup;
    }

    cJSON *root = cJSON_Parse(response_data);
    if (!root) {
        ESP_LOGW(TAG, "网关响应 JSON 解析失败: %.160s", response_data);
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    cJSON *answer_json = cJSON_GetObjectItemCaseSensitive(root, "answer");
    if (!cJSON_IsString(answer_json) || !answer_json->valuestring[0]) {
        ESP_LOGW(TAG, "网关响应没有 answer 字段");
        cJSON_Delete(root);
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    snprintf(answer, answer_size, "%s", answer_json->valuestring);
    cJSON_Delete(root);
    result = ESP_OK;

cleanup:
    esp_http_client_cleanup(client);
    free(post_data);
    free(response_data);
    return result;
}

static void llm_task(void *arg)
{
    llm_request_t request;
    char answer[LLM_ANSWER_MAX_BYTES];

    while (true) {
        if (xQueueReceive(s_request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        app_lcd_status_line(188, "正在思考", C_YELLOW);
        ESP_LOGI(TAG, "提问: %s", request.text);
        esp_err_t err = request_answer(request.text, answer, sizeof(answer));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "回答: %s", answer);
            app_lcd_show_llm_answer(answer);
            app_lcd_status_line(188, "正在播放", C_GREEN);
            esp_err_t play_err = app_speaker_play_text(answer);
            if (play_err == ESP_OK) {
                app_lcd_show_llm_answer(answer);
            } else {
                app_lcd_status_line(188, "语音播放失败", C_RED);
            }
        } else {
            app_lcd_status_line(188, "模型请求失败", C_RED);
        }
    }
}

void app_llm_start(void)
{
    if (s_request_queue) return;

    s_request_queue = xQueueCreate(1, sizeof(llm_request_t));
    if (!s_request_queue) {
        ESP_LOGE(TAG, "创建大模型请求队列失败");
        return;
    }
    if (xTaskCreate(llm_task, "llm_task", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建大模型任务失败");
        vQueueDelete(s_request_queue);
        s_request_queue = NULL;
        return;
    }
    ESP_LOGI(TAG, "大模型网关: %s", LLM_GATEWAY_URL);
}

void app_llm_ask(const char *text)
{
    if (!s_request_queue || !text || !text[0]) return;

    llm_request_t request = {0};
    snprintf(request.text, sizeof(request.text), "%s", text);
    xQueueOverwrite(s_request_queue, &request);
}

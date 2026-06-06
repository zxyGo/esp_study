#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "app_config.h"
#include "app_lcd.h"
#include "app_wifi.h"

static const char *TAG = "APP_WIFI";

#define WIFI_REPROVISION_RETRY_MAX 5
#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASS_KEY "pass"
#define WIFI_SCAN_MAX_AP 12

static volatile bool s_wifi_ok;
static volatile bool s_portal_active;
static bool s_wifi_started;
static int s_sta_retry_count;
static httpd_handle_t s_httpd;
static TaskHandle_t s_dns_task;
static int s_dns_sock = -1;

static bool load_saved_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) return false;

    size_t ssid_size = ssid_len;
    size_t pass_size = pass_len;
    err = nvs_get_str(nvs, WIFI_NVS_SSID_KEY, ssid, &ssid_size);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, WIFI_NVS_PASS_KEY, pass, &pass_size);
    }
    nvs_close(nvs);

    /* SSID 为空说明还没有配过网，或者旧配置已经被清除。 */
    return err == ESP_OK && ssid[0] != '\0';
}

static esp_err_t save_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs),
                        TAG, "打开 NVS 失败");
    ESP_ERROR_CHECK(nvs_set_str(nvs, WIFI_NVS_SSID_KEY, ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs, WIFI_NVS_PASS_KEY, pass));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    return ESP_OK;
}

static void erase_saved_wifi(void)
{
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void wifi_connect_sta(const char *ssid, const char *pass)
{
    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    s_wifi_ok = false;
    s_sta_retry_count = 0;
    app_lcd_status_line(44, "网络:连接中", C_ORANGE);

    /* APSTA 模式下：AP 继续给手机显示配网页，STA 同时去连接家里的路由器。 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(s_portal_active ? WIFI_MODE_APSTA : WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    } else {
        esp_wifi_disconnect();
        ESP_ERROR_CHECK(esp_wifi_connect());
    }
}

static void send_html_escaped(httpd_req_t *req, const char *text)
{
    for (const char *p = text; *p; p++) {
        switch (*p) {
        case '&':
            httpd_resp_sendstr_chunk(req, "&amp;");
            break;
        case '<':
            httpd_resp_sendstr_chunk(req, "&lt;");
            break;
        case '>':
            httpd_resp_sendstr_chunk(req, "&gt;");
            break;
        case '"':
            httpd_resp_sendstr_chunk(req, "&quot;");
            break;
        default:
            httpd_resp_send_chunk(req, p, 1);
            break;
        }
    }
}

static void send_portal_page(httpd_req_t *req, const char *message)
{
    wifi_ap_record_t records[WIFI_SCAN_MAX_AP] = {0};
    uint16_t ap_count = WIFI_SCAN_MAX_AP;

    /* 打开网页时扫描附近 2.4GHz Wi-Fi，生成可选择列表；如果扫描失败，仍可手动输入 SSID。 */
    esp_wifi_scan_start(NULL, true);
    esp_wifi_scan_get_ap_records(&ap_count, records);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ESP32-S3 配网</title>"
        "<style>"
        "body{font-family:Arial,'Microsoft YaHei',sans-serif;margin:0;background:#f6f7f9;color:#111}"
        ".wrap{max-width:420px;margin:36px auto;padding:0 18px}"
        ".box{background:white;border:1px solid #ddd;border-radius:8px;padding:20px}"
        "h1{font-size:22px;margin:0 0 8px}p{line-height:1.5;color:#555}"
        "label{display:block;margin-top:14px;font-weight:600}"
        "input{box-sizing:border-box;width:100%;font-size:16px;padding:11px;margin-top:6px;border:1px solid #bbb;border-radius:6px}"
        "button{width:100%;margin-top:18px;padding:12px;font-size:16px;border:0;border-radius:6px;background:#0b7cff;color:white}"
        ".msg{padding:10px;border-radius:6px;background:#fff4d6;color:#6b4b00;margin:12px 0}"
        ".hint{font-size:13px;color:#777}"
        "</style></head><body><main class=\"wrap\"><section class=\"box\">"
        "<h1>ESP32-S3 配网</h1><p>选择或输入家里的 2.4GHz Wi-Fi，然后填写密码。</p>");

    if (message && message[0]) {
        httpd_resp_sendstr_chunk(req, "<div class=\"msg\">");
        send_html_escaped(req, message);
        httpd_resp_sendstr_chunk(req, "</div>");
    }

    httpd_resp_sendstr_chunk(req,
        "<form method=\"post\" action=\"/save\">"
        "<label>选择家里的 Wi-Fi</label>"
        "<select name=\"ssid\" required "
        "style=\"box-sizing:border-box;width:100%;font-size:16px;padding:11px;margin-top:6px;border:1px solid #bbb;border-radius:6px;background:white\">");

    if (ap_count == 0) {
        httpd_resp_sendstr_chunk(req, "<option value=\"\">没有搜索到 Wi-Fi，请刷新页面重试</option>");
    } else {
        httpd_resp_sendstr_chunk(req, "<option value=\"\">请选择 Wi-Fi</option>");
    }

    for (int i = 0; i < ap_count; i++) {
        if (records[i].ssid[0] == '\0') continue;

        /* 扫描结果里同一个路由器可能出现多次，这里跳过重复 SSID，让初学者看到的列表更清爽。 */
        bool duplicate = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((const char *)records[i].ssid, (const char *)records[j].ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        httpd_resp_sendstr_chunk(req, "<option value=\"");
        send_html_escaped(req, (const char *)records[i].ssid);
        httpd_resp_sendstr_chunk(req, "\">");
        send_html_escaped(req, (const char *)records[i].ssid);
        char info[48];
        snprintf(info, sizeof(info), " (%ld dBm)%s", (long)records[i].rssi,
                 records[i].authmode == WIFI_AUTH_OPEN ? " 开放" : "");
        send_html_escaped(req, info);
        httpd_resp_sendstr_chunk(req, "</option>");
    }

    httpd_resp_sendstr_chunk(req,
        "</select><p class=\"hint\">列表来自开发板扫描到的 2.4GHz Wi-Fi，没看到就点刷新页面。</p>"
        "<label>Wi-Fi 密码</label>"
        "<input name=\"pass\" type=\"password\" maxlength=\"64\">"
        "<button type=\"submit\">保存并连接</button>"
        "<button type=\"button\" onclick=\"location.reload()\" "
        "style=\"background:#555;margin-top:10px\">重新搜索</button></form>"
        "<p class=\"hint\">如果没有自动弹出本页，请在浏览器打开 http://192.168.4.1</p>"
        "</section></main></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    send_portal_page(req, NULL);
    return ESP_OK;
}

static void url_decode(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 1 < dst_len; si++) {
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else if (src[si] == '%' && src[si + 1] && src[si + 2]) {
            char hex[3] = {src[si + 1], src[si + 2], 0};
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static bool form_get_value(const char *body, const char *key, char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *value = p + key_len + 1;
            const char *end = strchr(value, '&');
            size_t raw_len = end ? (size_t)(end - value) : strlen(value);
            char raw[160] = {0};
            if (raw_len >= sizeof(raw)) raw_len = sizeof(raw) - 1;
            memcpy(raw, value, raw_len);
            url_decode(out, out_len, raw);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int total = req->content_len;
    if (total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "表单太长");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret <= 0) return ESP_FAIL;
        received += ret;
    }
    body[received] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    if (!form_get_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        send_portal_page(req, "Wi-Fi 名称不能为空");
        return ESP_OK;
    }
    form_get_value(body, "pass", pass, sizeof(pass));

    ESP_LOGI(TAG, "网页收到 Wi-Fi 名称:%s", ssid);
    ESP_ERROR_CHECK(save_wifi(ssid, pass));
    wifi_connect_sta(ssid, pass);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>正在连接</title></head><body style=\"font-family:Arial;margin:32px\">"
        "<h2>已保存，正在连接家里的 Wi-Fi</h2>"
        "<p>请看开发板屏幕状态。连接成功后，设备会开始后续功能。</p>"
        "<p>如果一直失败，请返回上一页重新输入密码。</p>"
        "</body></html>");
    return ESP_OK;
}

static esp_err_t redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void start_http_server(void)
{
    if (s_httpd) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* 当前 sdkconfig 的 LWIP_MAX_SOCKETS 较小，HTTP 服务器内部还会占用 3 个 socket。
     * 配网页只服务手机上一两个连接，4 个外部 socket 已经足够。 */
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    ESP_ERROR_CHECK(httpd_start(&s_httpd, &config));

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
    };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_handler);
}

static int dns_reply_a_record(const char *request, int request_len, char *reply, size_t reply_len)
{
    if (request_len < 12 || request_len + 16 > (int)reply_len) return 0;

    memcpy(reply, request, request_len);
    reply[2] = 0x81; /* 标记为 DNS 响应 */
    reply[3] = 0x80; /* 无错误 */
    reply[6] = 0x00;
    reply[7] = 0x01; /* 1 个回答 */

    char *answer = reply + request_len;
    answer[0] = 0xC0;
    answer[1] = 0x0C; /* 指向问题里的域名 */
    answer[2] = 0x00;
    answer[3] = 0x01; /* A 记录 */
    answer[4] = 0x00;
    answer[5] = 0x01; /* IN */
    answer[6] = 0x00;
    answer[7] = 0x00;
    answer[8] = 0x00;
    answer[9] = 0x1E; /* TTL 30 秒 */
    answer[10] = 0x00;
    answer[11] = 0x04; /* IPv4 地址长度 */

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);
    memcpy(answer + 12, &ip_info.ip.addr, 4);
    return request_len + 16;
}

static void dns_task(void *arg)
{
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_dns_sock < 0 || bind(s_dns_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "DNS 服务启动失败");
        s_dns_task = NULL;
        vTaskDelete(NULL);
    }

    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(s_dns_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    while (s_portal_active) {
        char request[256];
        char reply[300];
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(s_dns_sock, request, sizeof(request), 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len <= 0) continue;

        int reply_len = dns_reply_a_record(request, len, reply, sizeof(reply));
        if (reply_len > 0) {
            sendto(s_dns_sock, reply, reply_len, 0, (struct sockaddr *)&source_addr, socklen);
        }
    }

    close(s_dns_sock);
    s_dns_sock = -1;
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void start_dns_server(void)
{
    if (!s_dns_task) {
        xTaskCreate(dns_task, "dns_server", 4096, NULL, 5, &s_dns_task);
    }
}

static void start_web_portal(bool reset_saved_wifi)
{
    if (reset_saved_wifi) erase_saved_wifi();

    s_wifi_ok = false;
    s_portal_active = true;
    s_sta_retry_count = 0;
    app_lcd_status_line(44, "网络:配网中", C_ORANGE);
    app_lcd_status_line(188, "连接热点配网", C_ORANGE);

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, WIFI_PORTAL_AP_SSID, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, WIFI_PORTAL_AP_PASS, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = strlen(WIFI_PORTAL_AP_SSID);
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = strlen(WIFI_PORTAL_AP_PASS) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    }

    ESP_LOGI(TAG, "网页配网热点已启动，SSID:%s，密码:%s，浏览器地址:http://192.168.4.1",
             WIFI_PORTAL_AP_SSID, WIFI_PORTAL_AP_PASS);
    start_http_server();
    start_dns_server();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_ok = false;
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;

        if (s_portal_active) {
            ESP_LOGW(TAG, "连接家里 Wi-Fi 失败，reason=%d，请在网页中重新填写", event->reason);
            app_lcd_status_line(44, "网络:配网中", C_ORANGE);
            return;
        }

        s_sta_retry_count++;
        if (s_sta_retry_count >= WIFI_REPROVISION_RETRY_MAX) {
            ESP_LOGW(TAG, "旧 Wi-Fi 连续连接失败，进入网页配网");
            start_web_portal(true);
        } else {
            ESP_LOGW(TAG, "Wi-Fi 断开，正在重连(%d/%d)", s_sta_retry_count, WIFI_REPROVISION_RETRY_MAX);
            app_lcd_status_line(44, "网络:重连", C_ORANGE);
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "手机已连接配网热点");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_ok = true;
        s_sta_retry_count = 0;
        ESP_LOGI(TAG, "Wi-Fi 已连接，IP:" IPSTR, IP2STR(&event->ip_info.ip));
        app_lcd_status_line(44, "网络:已连接", C_GREEN);
    }
}

void app_wifi_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS 用来保存网页配网得到的 SSID/密码；分区异常时擦除后重新初始化。 */
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    char ssid[33] = {0};
    char pass[65] = {0};
    if (load_saved_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "检测到已保存的 Wi-Fi，直接连接:%s", ssid);
        wifi_connect_sta(ssid, pass);
    } else {
        start_web_portal(false);
    }
}

bool app_wifi_is_connected(void)
{
    return s_wifi_ok;
}

void app_wifi_wait_connected(void)
{
    /* 若一直停在这里，请连接 ESP32S3_STT 热点，在弹出的网页里填写家里 Wi-Fi。 */
    while (!app_wifi_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

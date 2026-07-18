#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_gps.h"
#include "app_lcd.h"

#define GPS_UART_RX_BUFFER_SIZE 1024
#define GPS_READ_CHUNK_SIZE     128
#define GPS_SENTENCE_MAX_SIZE   128
#define GPS_BAUD_PROBE_MS       4000

static const char *TAG = "APP_GPS";
static const int s_baud_rates[] = {GPS_BAUD_RATE, 38400, 115200, 4800};

typedef struct {
    double latitude;
    double longitude;
    char utc[9];
    bool fix_valid;
    bool time_valid;
} gps_state_t;

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool checksum_valid(const char *sentence)
{
    const char *star;
    uint8_t checksum = 0;
    int high;
    int low;

    if (!sentence || sentence[0] != '$') return false;
    star = strchr(sentence, '*');
    if (!star || star[1] == '\0' || star[2] == '\0') return false;

    for (const char *p = sentence + 1; p < star; ++p) {
        checksum ^= (uint8_t)*p;
    }
    high = hex_value(star[1]);
    low = hex_value(star[2]);
    return high >= 0 && low >= 0 && checksum == (uint8_t)((high << 4) | low);
}

static int split_fields(char *sentence, char **fields, int max_fields)
{
    int count = 0;
    char *p = sentence + 1;

    while (count < max_fields) {
        fields[count++] = p;
        while (*p && *p != ',') ++p;
        if (!*p) break;
        *p++ = '\0';
    }
    return count;
}

static bool sentence_type_is(const char *field, const char *type)
{
    size_t len = strlen(field);
    return len >= 3 && strcmp(field + len - 3, type) == 0;
}

static bool parse_utc(const char *raw, char utc[9])
{
    if (!raw || strlen(raw) < 6) return false;
    for (int i = 0; i < 6; ++i) {
        if (!isdigit((unsigned char)raw[i])) return false;
    }

    int hour = (raw[0] - '0') * 10 + raw[1] - '0';
    int minute = (raw[2] - '0') * 10 + raw[3] - '0';
    int second = (raw[4] - '0') * 10 + raw[5] - '0';
    if (hour > 23 || minute > 59 || second > 60) return false;

    utc[0] = raw[0];
    utc[1] = raw[1];
    utc[2] = ':';
    utc[3] = raw[2];
    utc[4] = raw[3];
    utc[5] = ':';
    utc[6] = raw[4];
    utc[7] = raw[5];
    utc[8] = '\0';
    return true;
}

static bool parse_coordinate(const char *raw, const char *hemisphere,
                             bool is_latitude, double *coordinate)
{
    char *end = NULL;
    double value;
    int degrees;
    double minutes;
    char hemi;

    if (!raw || !raw[0] || !hemisphere || !hemisphere[0] || !coordinate) {
        return false;
    }

    value = strtod(raw, &end);
    if (end == raw || *end != '\0' || value < 0.0) return false;
    degrees = (int)(value / 100.0);
    minutes = value - degrees * 100.0;
    if (minutes < 0.0 || minutes >= 60.0) return false;

    *coordinate = degrees + minutes / 60.0;
    if ((is_latitude && *coordinate > 90.0) ||
        (!is_latitude && *coordinate > 180.0)) {
        return false;
    }

    hemi = (char)toupper((unsigned char)hemisphere[0]);
    if (hemi == 'S' || hemi == 'W') {
        *coordinate = -*coordinate;
    } else if ((is_latitude && hemi != 'N') ||
               (!is_latitude && hemi != 'E')) {
        return false;
    }
    return true;
}

static bool parse_position(char **fields, int field_count, int lat_index,
                           int lon_index, double *latitude, double *longitude)
{
    return field_count > lon_index + 1 &&
           parse_coordinate(fields[lat_index], fields[lat_index + 1], true,
                            latitude) &&
           parse_coordinate(fields[lon_index], fields[lon_index + 1], false,
                            longitude);
}

static bool parse_nmea(char *sentence, gps_state_t *state)
{
    char *fields[20];
    char *star;
    int field_count;
    double latitude;
    double longitude;

    if (!checksum_valid(sentence)) return false;
    star = strchr(sentence, '*');
    *star = '\0';
    field_count = split_fields(sentence, fields,
                               sizeof(fields) / sizeof(fields[0]));
    if (field_count < 2) return false;

    if (sentence_type_is(fields[0], "RMC")) {
        state->time_valid = parse_utc(fields[1], state->utc);
        state->fix_valid = field_count > 6 && fields[2][0] == 'A' &&
                           parse_position(fields, field_count, 3, 5,
                                          &latitude, &longitude);
        if (state->fix_valid) {
            state->latitude = latitude;
            state->longitude = longitude;
        }
        return true;
    }

    if (sentence_type_is(fields[0], "GGA")) {
        state->time_valid = parse_utc(fields[1], state->utc);
        state->fix_valid = field_count > 6 && atoi(fields[6]) > 0 &&
                           parse_position(fields, field_count, 2, 4,
                                          &latitude, &longitude);
        if (state->fix_valid) {
            state->latitude = latitude;
            state->longitude = longitude;
        }
        return true;
    }

    return false;
}

static void gps_task(void *arg)
{
    uint8_t data[GPS_READ_CHUNK_SIZE];
    char sentence[GPS_SENTENCE_MAX_SIZE];
    size_t sentence_length = 0;
    bool receiving = false;
    gps_state_t state = {0};
    size_t baud_index = 0;
    uint32_t received_bytes = 0;
    bool baud_locked = false;
    TickType_t probe_started = xTaskGetTickCount();

    (void)arg;
    while (true) {
        int length = uart_read_bytes(GPS_UART, data, sizeof(data),
                                     pdMS_TO_TICKS(250));
        if (length > 0) received_bytes += (uint32_t)length;

        for (int i = 0; i < length; ++i) {
            char c = (char)data[i];

            if (c == '$') {
                sentence[0] = c;
                sentence_length = 1;
                receiving = true;
                continue;
            }
            if (!receiving) continue;

            if (c == '\n') {
                sentence[sentence_length] = '\0';
                if (parse_nmea(sentence, &state)) {
                    if (!baud_locked) {
                        baud_locked = true;
                        ESP_LOGI(TAG, "valid NMEA detected, baud locked at %d",
                                 s_baud_rates[baud_index]);
                    }
                    app_lcd_set_gps(state.latitude, state.longitude,
                                    state.time_valid ? state.utc : NULL,
                                    state.fix_valid);
                    if (state.fix_valid) {
                        ESP_LOGI(TAG, "fix lat=%.6f lon=%.6f UTC=%s",
                                 state.latitude, state.longitude,
                                 state.time_valid ? state.utc : "--:--:--");
                    }
                }
                sentence_length = 0;
                receiving = false;
            } else if (c != '\r') {
                if (sentence_length < sizeof(sentence) - 1) {
                    sentence[sentence_length++] = c;
                } else {
                    sentence_length = 0;
                    receiving = false;
                    ESP_LOGW(TAG, "NMEA sentence is too long, discarded");
                }
            }
        }

        TickType_t now = xTaskGetTickCount();
        if (!baud_locked &&
            (now - probe_started) >= pdMS_TO_TICKS(GPS_BAUD_PROBE_MS)) {
            if (received_bytes == 0) {
                ESP_LOGW(TAG, "baud %d: no bytes received on GPIO%d",
                         s_baud_rates[baud_index], GPS_RX);
            } else {
                ESP_LOGW(TAG, "baud %d: received %lu bytes, no valid RMC/GGA",
                         s_baud_rates[baud_index], (unsigned long)received_bytes);
            }

            baud_index = (baud_index + 1) %
                         (sizeof(s_baud_rates) / sizeof(s_baud_rates[0]));
            ESP_ERROR_CHECK(uart_flush_input(GPS_UART));
            ESP_ERROR_CHECK(uart_set_baudrate(GPS_UART,
                                               s_baud_rates[baud_index]));
            ESP_LOGI(TAG, "probing GPS baud %d", s_baud_rates[baud_index]);
            received_bytes = 0;
            sentence_length = 0;
            receiving = false;
            probe_started = now;
        }
    }
}

void app_gps_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = GPS_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(GPS_UART, GPS_UART_RX_BUFFER_SIZE,
                                        0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART, GPS_TX, GPS_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    app_lcd_set_gps(0.0, 0.0, NULL, false);
    if (xTaskCreate(gps_task, "gps_nmea", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create GPS task");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    ESP_LOGI(TAG, "GPS UART ready: RX=GPIO%d TX=GPIO%d baud=%d",
             GPS_RX, GPS_TX, GPS_BAUD_RATE);
}

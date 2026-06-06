#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "app_config.h"
#include "app_mic.h"

static const char *TAG = "APP_MIC";

static i2s_chan_handle_t s_rx;
static int32_t s_pcm[BUF_SAMPLES];

void app_mic_init(void)
{
    /* 创建 I2S 接收通道：INMP441 只需要 RX，不需要 TX。 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx));

    /* INMP441 输出 24-bit 数据，左对齐放在 32-bit 字里。 */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(STT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_BCLK,
            .ws = MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_DIN,
        },
    };

    /* 同时读取左右声道，避免 L/R 接 GND 或 3V3 不同导致读不到声音。 */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
}

int app_mic_read_pcm16(int16_t *out, int max_frames, int32_t *peak_out)
{
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(s_rx, s_pcm, sizeof(s_pcm), &bytes_read,
                                     pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2S 读取失败: %s，请检查 BCLK=%d、WS=%d、DIN=%d 接线",
                 esp_err_to_name(ret), MIC_BCLK, MIC_WS, MIC_DIN);
        if (peak_out) *peak_out = 0;
        return 0;
    }

    int total_samples = (int)(bytes_read / sizeof(int32_t));
    int frames = total_samples / 2;
    if (frames > max_frames) frames = max_frames;

    int32_t peak = 0;
    for (int i = 0; i < frames; i++) {
        int32_t left24 = s_pcm[i * 2] >> 8;
        int32_t right24 = s_pcm[i * 2 + 1] >> 8;

        /* INMP441 通常只在一个声道有数据，这里自动取幅度更大的声道。 */
        int32_t sample24 = (abs(left24) > abs(right24)) ? left24 : right24;
        int32_t sample16 = sample24 >> 8;

        if (sample16 > 32767) sample16 = 32767;
        if (sample16 < -32768) sample16 = -32768;

        out[i] = (int16_t)sample16;
        int32_t v = abs(sample16);
        if (v > peak) peak = v;
    }

    if (peak_out) *peak_out = peak;
    return frames;
}

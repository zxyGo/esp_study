#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "app_config.h"
#include "app_lcd.h"
#include "app_mic.h"
#include "app_wifi.h"
#include "app_stt.h"
#include "app_llm.h"
#include "app_speaker.h"

static const char *TAG = "APP_MAIN";

void app_main(void)
{
    /* main.c 只负责整体流程：初始化硬件、联网、启动语音转写、循环发送音频。 */
    app_lcd_init();
    app_mic_init();

    app_lcd_show_boot();
    vTaskDelay(pdMS_TO_TICKS(2000));

    app_lcd_draw_stt_frame();
    /* Wi-Fi 初始化会先初始化 NVS，扬声器随后从中恢复上次保存的音量。 */
    app_wifi_init();
    app_speaker_init();
    app_lcd_set_speaker_volume(app_speaker_get_volume());

    int16_t pcm[STT_CHUNK_FRAMES];
    /* 音量条使用一个会自动回落的参考峰值。旧实现记录“开机以来最大值”且永不
     * 衰减，INMP441 上电时的一次瞬态尖峰就可能把基准锁在 32767，导致之后
     * 正常说话仍因整数取整而一直显示 0%。 */
    int32_t level_reference = VOLUME_INITIAL_FULL_SCALE;
    int32_t display_peak = 0;
    char buf[24];
    TickType_t last_commit = xTaskGetTickCount();
    TickType_t last_ui = 0;
    bool stt_started = false;

    while (1) {
        int32_t peak = 0;
        int frames = app_mic_read_pcm16(pcm, STT_CHUNK_FRAMES, &peak);

        /* 不等待 Wi-Fi 也持续读取麦克风，方便先确认 INMP441 接线和音量是否正常。
         * Wi-Fi 连上后再启动语音识别与大模型后台任务；API Key 只保存在 PC 网关。 */
        if (!stt_started && app_wifi_is_connected()) {
            app_llm_start();
            app_stt_start();
            stt_started = true;
        }

        /* 保存本次 UI 刷新周期内的最大值，避免只显示恰好采到的最后 30 ms。 */
        if (peak > display_peak) display_peak = peak;
        /* 播放回答时不把扬声器声音送回 FunASR，避免形成自问自答循环。 */
        if (!app_speaker_is_playing()) {
            app_stt_send_audio_chunk(pcm, frames);
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_commit) >= pdMS_TO_TICKS(STT_COMMIT_MS)) {
            app_stt_commit_audio();
            last_commit = now;
        }

        /* 屏幕刷新比音频采集慢很多，300 ms 更新一次即可观察麦克风状态。 */
        if ((now - last_ui) >= pdMS_TO_TICKS(300)) {
            if (display_peak > level_reference) {
                level_reference = display_peak;
            } else {
                /* 每 300 ms 回落 25%，约 3 秒即可从满量程尖峰恢复到正常范围。 */
                level_reference = level_reference * 3 / 4;
                if (level_reference < VOLUME_INITIAL_FULL_SCALE) {
                    level_reference = VOLUME_INITIAL_FULL_SCALE;
                }
            }

            int pct = (int)((int64_t)display_peak * 100 / level_reference);
            if (pct > 100) pct = 100;
            /* 有真实采样但幅度低于 1% 时仍显示 1；全零数据继续显示红色 0。 */
            if (display_peak > 0 && pct == 0) pct = 1;
            snprintf(buf, sizeof(buf), "麦克风:%3d%%", pct);
            app_lcd_status_line(116, buf, display_peak == 0 ? C_RED : C_GREEN);
            ESP_LOGI(TAG, "麦克风: frames=%d peak=%ld ref=%ld pct=%d%%",
                     frames, (long)display_peak, (long)level_reference, pct);
            display_peak = 0;
            last_ui = now;
        }
    }
}

#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* 初始化 MAX98357A 对应的 I2S TX 通道。 */
void app_speaker_init(void);

/* 请求局域网离线 TTS，完整缓存 PCM 后连续播放。此函数会阻塞调用它的 LLM 任务。 */
esp_err_t app_speaker_play_text(const char *text);

/* 从发起 TTS 到播放结束后的回声保护期内均返回 true。 */
bool app_speaker_is_playing(void);

/* 设置/读取播放音量百分比。设置值会限制到 0~100 并保存到 NVS。 */
esp_err_t app_speaker_set_volume(int percent);
int app_speaker_get_volume(void);

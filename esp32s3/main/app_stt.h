#pragma once

#include <stdint.h>
#include <stdbool.h>

void app_stt_start(void);
bool app_stt_is_connected(void);
void app_stt_send_audio_chunk(const int16_t *pcm, int frames);
void app_stt_commit_audio(void);

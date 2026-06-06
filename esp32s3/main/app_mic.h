#pragma once

#include <stdint.h>

void app_mic_init(void);
int app_mic_read_pcm16(int16_t *out, int max_frames, int32_t *peak_out);

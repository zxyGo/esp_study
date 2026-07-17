#pragma once

#include <stdint.h>
#include "app_config.h"

void app_lcd_init(void);

void app_lcd_show_boot(void);
void app_lcd_draw_stt_frame(void);
void app_lcd_status_line(int y, const char *text, uint16_t color);
void app_lcd_show_wake_hint(void);
void app_lcd_show_transcript_hint(const char *text);
void app_lcd_show_llm_answer(const char *text);
void app_lcd_set_speaker_volume(int percent);

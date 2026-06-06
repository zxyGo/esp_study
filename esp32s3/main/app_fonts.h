#pragma once

#include "lvgl.h"

/* 项目自定义中文字体。
 * 字库由 main/fonts/app_chinese_chars.txt 控制，只包含教程里常用的汉字和标点。
 * 如果屏幕上某个汉字显示不出来，把那个字加入字符集文件后重新生成字体即可。 */
LV_FONT_DECLARE(app_font_chinese_16);

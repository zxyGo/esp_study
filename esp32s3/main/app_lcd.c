#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "app_fonts.h"
#include "app_lcd.h"

static const char *TAG = "APP_LVGL";

static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;

static lv_obj_t *s_title;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_ws_label;
static lv_obj_t *s_mic_label;
static lv_obj_t *s_volume_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_text_label;

static void apply_chinese_font(lv_obj_t *obj)
{
    /* LVGL 的内置 Montserrat 字体不含中文。
     * 这里统一给 label 设置项目自定义字体，后面新增中文只需要扩展字库。 */
    lv_obj_set_style_text_font(obj, &app_font_chinese_16, 0);
}

static lv_color_t color_from_rgb565(uint16_t color)
{
    switch (color) {
    case C_RED:
        return lv_palette_main(LV_PALETTE_RED);
    case C_GREEN:
        return lv_palette_main(LV_PALETTE_GREEN);
    case C_YELLOW:
        return lv_palette_main(LV_PALETTE_YELLOW);
    case C_CYAN:
        return lv_palette_main(LV_PALETTE_CYAN);
    case C_ORANGE:
        return lv_palette_main(LV_PALETTE_ORANGE);
    case C_WHITE:
        return lv_color_white();
    case C_BLACK:
        return lv_color_black();
    default:
        return lv_color_black();
    }
}

static void label_set(lv_obj_t *label, const char *text, uint16_t color)
{
    if (!label) return;

    if (lvgl_port_lock(0)) {
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, color_from_rgb565(color), 0);
        lvgl_port_unlock();
    }
}

static void create_label(lv_obj_t **label, const char *text, int y, uint16_t color)
{
    *label = lv_label_create(lv_scr_act());
    apply_chinese_font(*label);
    lv_label_set_text(*label, text);
    lv_obj_set_width(*label, LCD_W - 16);
    lv_obj_set_style_text_align(*label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(*label, color_from_rgb565(color), 0);
    lv_obj_align(*label, LV_ALIGN_TOP_LEFT, 8, y);
}

void app_lcd_init(void)
{
    gpio_set_direction(LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL, 1);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_DC,
        .cs_gpio_num = LCD_CS,
        .pclk_hz = LCD_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI,
                                             &io_cfg, &io_handle));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = s_panel,
        .buffer_size = LCD_W * 40,
        .double_buffer = true,
        .hres = LCD_W,
        .vres = LCD_H,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_ERROR_CHECK(s_disp ? ESP_OK : ESP_FAIL);

    ESP_LOGI(TAG, "LVGL 屏幕初始化完成");
}

void app_lcd_show_boot(void)
{
    if (lvgl_port_lock(0)) {
        lv_obj_t *scr = lv_scr_act();
        lv_obj_clean(scr);
        // 背景颜色用白色，文字用黑色和蓝绿色，形成鲜明对比，同时也能测试屏幕的颜色显示是否正常。
        lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

        lv_obj_t *hello = lv_label_create(scr);
        apply_chinese_font(hello);
        lv_label_set_text(hello, "启动中");
        lv_obj_set_style_text_color(hello, lv_color_black(), 0);
        lv_obj_align(hello, LV_ALIGN_CENTER, 0, -28);

        lv_obj_t *sub = lv_label_create(scr);
        apply_chinese_font(sub);
        lv_label_set_text(sub, "本地语音助手");
        lv_obj_set_style_text_color(sub, lv_palette_main(LV_PALETTE_CYAN), 0);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 28);

        lvgl_port_unlock();
    }
}

void app_lcd_draw_stt_frame(void)
{
    if (lvgl_port_lock(0)) {
        lv_obj_t *scr = lv_scr_act();
        lv_obj_clean(scr);
        lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

        s_title = lv_label_create(scr);
        apply_chinese_font(s_title);
        lv_label_set_text(s_title, "语音助手");
        lv_obj_set_style_text_color(s_title, lv_palette_main(LV_PALETTE_CYAN), 0);
        lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 4);

        create_label(&s_wifi_label, "网络:等待", 30, C_BLACK);
        create_label(&s_ws_label, "服务:等待", 54, C_BLACK);
        create_label(&s_mic_label, "麦克风:  0%", 78, C_GREEN);
        lv_obj_set_width(s_mic_label, 116);

        s_volume_label = lv_label_create(scr);
        apply_chinese_font(s_volume_label);
        lv_label_set_text(s_volume_label, "音量:100%");
        lv_obj_set_width(s_volume_label, 108);
        lv_obj_set_style_text_align(s_volume_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(s_volume_label,
                                    lv_palette_main(LV_PALETTE_CYAN), 0);
        lv_obj_align(s_volume_label, LV_ALIGN_TOP_RIGHT, -8, 78);
        create_label(&s_hint_label, "请说：你好，禹神", 216, C_ORANGE);

        s_text_label = lv_label_create(scr);
        apply_chinese_font(s_text_label);
        lv_label_set_text(s_text_label, "");
        lv_obj_set_width(s_text_label, LCD_W - 16);
        lv_obj_set_height(s_text_label, 104);
        lv_label_set_long_mode(s_text_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(s_text_label, lv_palette_main(LV_PALETTE_ORANGE), 0);
        lv_obj_align(s_text_label, LV_ALIGN_TOP_LEFT, 8, 106);

        lvgl_port_unlock();
    }
}

void app_lcd_status_line(int y, const char *text, uint16_t color)
{
    if (y == 44) {
        label_set(s_wifi_label, text, color);
    } else if (y == 80) {
        label_set(s_ws_label, text, color);
    } else if (y == 116) {
        label_set(s_mic_label, text, color);
    } else if (y == 188) {
        label_set(s_hint_label, text, color);
    }
}

void app_lcd_show_wake_hint(void)
{
    if (lvgl_port_lock(0)) {
        if (s_text_label) {
            lv_label_set_text(s_text_label, WAKE_WORD_DISPLAY);
            lv_obj_set_style_text_color(s_text_label, lv_palette_main(LV_PALETTE_GREEN), 0);
        }
        if (s_hint_label) {
            lv_label_set_text(s_hint_label, "已唤醒，请提问");
            lv_obj_set_style_text_color(s_hint_label, lv_palette_main(LV_PALETTE_GREEN), 0);
        }
        lvgl_port_unlock();
    }
}

void app_lcd_show_transcript_hint(const char *text)
{
    /* text 是 UTF-8 字符串，字体已包含 GB2312 一级常用汉字。
     * 生僻字仍可加入 main/fonts/app_chinese_chars.txt 后重新生成字体。 */
    if (lvgl_port_lock(0)) {
        if (s_text_label) {
            lv_label_set_text(s_text_label, text);
            lv_obj_set_style_text_color(s_text_label, lv_palette_main(LV_PALETTE_ORANGE), 0);
        }
        if (s_hint_label) {
            lv_label_set_text(s_hint_label, "收到问题");
        }
        lvgl_port_unlock();
    }
}

void app_lcd_show_llm_answer(const char *text)
{
    if (lvgl_port_lock(0)) {
        if (s_text_label) {
            lv_label_set_text(s_text_label, text);
            lv_obj_set_style_text_color(s_text_label, lv_palette_main(LV_PALETTE_CYAN), 0);
        }
        if (s_hint_label) {
            lv_label_set_text(s_hint_label, "请说：你好，禹神");
            lv_obj_set_style_text_color(s_hint_label, lv_palette_main(LV_PALETTE_GREEN), 0);
        }
        lvgl_port_unlock();
    }
}

void app_lcd_set_speaker_volume(int percent)
{
    char text[20];
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    snprintf(text, sizeof(text), "音量:%3d%%", percent);
    label_set(s_volume_label, text, percent == 0 ? C_RED : C_CYAN);
}

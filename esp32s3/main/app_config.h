#pragma once

#include "driver/gpio.h"
#include "driver/spi_common.h"

/* ================================================================
 * 硬件引脚配置
 * ================================================================ */

/* INMP441 麦克风引脚：如果你的接线不同，只改这里。 */
#define MIC_BCLK  GPIO_NUM_5   /* I2S 位时钟 BCLK / SCK */
#define MIC_WS    GPIO_NUM_4   /* I2S 左右声道时钟 WS / LRCLK */
#define MIC_DIN   GPIO_NUM_6   /* 麦克风数据输出 SD / DOUT，接 ESP32-S3 输入 */

/* MAX98357A I2S 数字功放。DIN 是功放的数据输入，对应 ESP32-S3 的输出。 */
#define SPEAKER_BCLK GPIO_NUM_15
#define SPEAKER_WS   GPIO_NUM_16
#define SPEAKER_DOUT GPIO_NUM_7   /* 按实际接线图连接 MAX98357A DIN */
#define SPEAKER_SD   GPIO_NUM_18  /* MAX98357A SD/MODE，高电平使能功放 */

/* ST7789 屏幕引脚：SPI 屏幕只需要 MOSI，不需要 MISO。 */
#define LCD_SCLK  GPIO_NUM_21  /* SPI 时钟 SCL / SCK */
#define LCD_MOSI  GPIO_NUM_47  /* SPI 数据 SDA / MOSI */
#define LCD_CS    GPIO_NUM_41  /* 片选 CS，低电平有效 */
#define LCD_DC    GPIO_NUM_40  /* 数据/命令选择 D/C */
#define LCD_RST   GPIO_NUM_45  /* 复位 RST / RES */
#define LCD_BL    GPIO_NUM_42  /* 背光 BLK / BL，高电平点亮 */

/* ================================================================
 * 屏幕与颜色配置
 * ================================================================ */

#define LCD_W    240
#define LCD_H    240
#define LCD_HZ   (10 * 1000 * 1000)  /* SPI 10 MHz，杜邦线较长时更稳定 */
#define LCD_SPI  SPI2_HOST

/* RGB565 颜色值已经做了高低字节交换，方便直接通过 SPI 发送给 ST7789。 */
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_GREEN   0xE007
#define C_RED     0x00F8
#define C_YELLOW  0xE0FF
#define C_CYAN    0xFF07
#define C_ORANGE  0x20FD

/* ================================================================
 * 音频与实时转写配置
 * ================================================================ */

/* FunASR 服务端要求 16 kHz 单声道 PCM16。 */
#define STT_SAMPLE_RATE      16000
#define STT_CHUNK_FRAMES     480   /* 480 帧 = 30 ms @ 16 kHz */
#define STT_COMMIT_MS        5000  /* 主循环调用节奏保留；FunASR 靠 VAD 自动断句 */
#define BUF_SAMPLES          (STT_CHUNK_FRAMES * 2) /* I2S 读左右双声道 */
#define VOLUME_INITIAL_FULL_SCALE  2000

/* ================================================================
 * Wi-Fi 网页配网配置
 * ================================================================ */

/* 首次启动时，开发板会创建这个临时热点；手机连接后会弹出或手动打开配网页。 */
#define WIFI_PORTAL_AP_SSID "ESP32S3_STT"
/* 配网热点密码，至少 8 位；手机连接这个热点时会用到。 */
#define WIFI_PORTAL_AP_PASS "12345678"

/* FunASR 本地服务地址（无需 API Key）。
 * 先在 PC 上执行 cd server && docker compose up -d，
 * 再把下面的 IP 换成运行 Docker 的那台机器的局域网 IP。 */
#define FUNASR_WS_URL  "ws://10.10.1.73:10095"

/* 大模型请求经由 PC 上的局域网网关转发，API Key 不会写入固件。
 * IP 与 FUNASR_WS_URL 使用同一台运行 Docker Compose 的电脑。 */
#define LLM_GATEWAY_URL        "http://10.10.1.73:10096/chat"
#define LLM_SESSION_ID         "esp32s3-voice-assistant"
#define LLM_REQUEST_TIMEOUT_MS 70000
#define LLM_PROMPT_MAX_BYTES   256
#define LLM_RESPONSE_MAX_BYTES 4096
#define LLM_ANSWER_MAX_BYTES   1024

/* 离线 MeloTTS 模型输出 44.1 kHz、单声道 PCM16；固件会复制到左右声道。 */
#define TTS_GATEWAY_URL          "http://10.10.1.73:10096/tts"
#define TTS_SAMPLE_RATE          44100
#define TTS_REQUEST_TIMEOUT_MS   70000
#define TTS_MAX_AUDIO_BYTES      (4 * 1024 * 1024)
#define SPEAKER_ECHO_GUARD_MS    350

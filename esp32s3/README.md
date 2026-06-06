# ESP32-S3 Voice Display Starter

## 当前新增：实时语音转文字

本项目现在加入了“云端实时语音转文字”骨架：

1. ESP32-S3 通过 INMP441 采集 24 kHz 音频。
2. 程序把 I2S 原始数据转换为 16-bit 单声道 PCM。
3. 通过 Wi-Fi 连接 OpenAI Realtime Transcription WebSocket。
4. 每 1 秒提交一段音频，串口打印实时片段和最终文字。
5. ST7789 屏幕显示 Wi-Fi、WebSocket、麦克风音量和“识别到文字”的状态。

使用前请先修改 `main/app_config.h` 里的三项配置：

```c
#define WIFI_SSID      "请改成你的WiFi名称"
#define WIFI_PASS      "请改成你的WiFi密码"
#define OPENAI_API_KEY "请改成你的OpenAI_API_Key"
```

屏幕已经接入 LVGL 中文字体，可以直接显示项目字库内包含的中文。中文转写结果仍会完整打印到串口监视器里；如果屏幕上某个汉字没有显示出来，把那个字加入 `main/fonts/app_chinese_chars.txt` 后重新生成字体即可。

编译：

```powershell
cmd.exe /c "C:\esp\v5.4.4\esp-idf\export.bat && idf.py build"
```

烧录和监视串口：

```powershell
cmd.exe /c "C:\esp\v5.4.4\esp-idf\export.bat && idf.py -p COM4 flash monitor"
```

如果你的串口不是 `COM4`，请按实际设备管理器里的端口修改。

## 当前代码结构

现在代码已经从单个 `main.c` 拆成多个模块：

```text
main/
├── main.c              // 程序入口，只负责整体启动流程
├── app_config.h        // 引脚、Wi-Fi、API Key、采样率等公共配置
├── app_lcd.c/.h        // ST7789 初始化，并通过 LVGL 显示界面
├── app_fonts.h         // LVGL 中文字体声明
├── fonts/              // 项目自定义中文字符集和生成后的字体 C 文件
├── app_mic.c/.h        // INMP441 / I2S 初始化和 PCM16 音频读取
├── app_wifi.c/.h       // Wi-Fi 连接和重连
├── app_stt.c/.h        // OpenAI Realtime WebSocket 和语音转文字
├── CMakeLists.txt      // ESP-IDF 组件构建配置
└── idf_component.yml   // 第三方托管组件依赖
```

这样 `main.c` 会更接近真实项目写法：它只串起流程，具体硬件和网络细节放到各自模块里。

## LVGL 说明

屏幕层已经切换到 LVGL：

- `app_lcd.c` 仍然用 ESP-IDF 官方 `esp_lcd` 初始化 ST7789。
- 刷屏、界面对象、标签文本更新交给 `esp_lvgl_port` 和 LVGL。
- 当前 LVGL 版本由组件管理器解析为 `lvgl/lvgl 8.4.0`。
- 当前项目使用 `main/fonts/app_font_chinese_16.c` 里的 16px 中文字体。

字体生成方式：

```powershell
powershell.exe -ExecutionPolicy Bypass -File tools\gen_lvgl_font.ps1
```

这个脚本会读取 `main/fonts/app_chinese_chars.txt`，使用 LVGL 组件自带的 `SimSun.woff` 生成 `main/fonts/app_font_chinese_16.c`。

注意：ESP32-S3 的 Flash 和 app 分区有限，不建议一开始就生成全量中文字库。教程项目先使用“项目需要哪些字，就生成哪些字”的方式；后续如果发现某个字缺失，只要加入字符集文件并重新运行脚本即可。

LVGL 会让固件变大，所以项目已在 `sdkconfig.defaults` 里启用大 app 分区：

```text
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp_large.csv"
```

本项目先从最稳的一步开始：使用 ESP32-S3 读取 INMP441 麦克风，在串口打印声音大小，并把音量显示到 ST7789 屏幕上。

当前已经包含：

1. ST7789 屏幕显示文字
2. 麦克风音量显示到屏幕

后续再逐步加入：

1. MAX98357A 扬声器播放声音
2. 语音命令识别
3. 如果需要完整语音转文字，再接云端语音识别

## 你的硬件

- 开发板：ESP32-S3
- 麦克风：INMP441
- 音频功放：MAX98357A
- 屏幕：ST7789

## 先理解整体路线

不要一开始就做“说一句话，屏幕显示完整文字”。这个目标包含麦克风、屏幕、网络、语音识别、内存管理等很多环节，初学者很容易不知道错在哪里。

推荐顺序是：

1. 麦克风能采到声音
2. 屏幕能显示固定文字
3. 麦克风音量显示到屏幕
4. 识别几个固定命令，并显示对应文字
5. 最后再做完整语音转文字

当前代码已经走到第 5 步的骨架：串口打印麦克风音量，ST7789 通过 LVGL 显示中文状态，并通过云端实时语音转文字服务返回识别结果。

## 第 1 步：INMP441 麦克风测试

### 接线

当前 `main/app_config.h` 使用下面的 GPIO：

| INMP441 引脚 | ESP32-S3 引脚 | 说明 |
| --- | --- | --- |
| VCC | 3V3 | 不要接 5V |
| GND | GND | 共地 |
| SCK / BCLK | GPIO5 | I2S 位时钟 |
| WS / LRCLK | GPIO4 | I2S 左右声道时钟 |
| SD / DOUT | GPIO6 | 麦克风数据输出 |
| L/R | GND 或 3V3 | 选择左/右声道；当前代码同时读取左右声道，方便排查 |

如果你的线接到别的 GPIO，修改 `main/app_config.h` 里的这三行：

```c
#define MIC_I2S_BCLK GPIO_NUM_5
#define MIC_I2S_WS   GPIO_NUM_4
#define MIC_I2S_DIN  GPIO_NUM_6
```

当前代码使用：

```c
std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
```

也就是同时读取左、右声道。INMP441 只会在其中一个声道输出数据，另一个声道通常是 0；这样做对初学者更友好，不容易因为 `L/R` 接法不同而读不到声音。

### 编译和烧录

在 ESP-IDF 终端中执行：

```powershell
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

如果串口不是自动识别，可以指定端口，例如：

```powershell
idf.py -p COM5 flash monitor
```

退出串口监视器：

```text
Ctrl + ]
```

### 成功现象

串口会持续打印类似：

```text
I (1234) INMP441_TEST: volume avg=120 peak=430 bytes=2048
```

你靠近麦克风说话、拍手时，`avg` 和 `peak` 应该明显变大。

屏幕会显示 `麦克风: xx%`。靠近麦克风说话、拍手时，百分比应该明显变化。

如果数值一直是 0，重点检查：

- VCC 是否接 3V3
- GND 是否共地
- SCK、WS、SD 是否接反
- 屏幕上的 `麦克风` 数值一直很低或一直为 0 时，优先检查 INMP441 的供电和三根 I2S 信号线

## 第 2 步：ST7789 屏幕显示文字

当前 `main/app_lcd.c` 已经加入 ST7789 + LVGL 初始化代码，烧录后屏幕会先短暂显示“启动中 / 实时语音转文字”，然后切换到语音转文字状态界面。

ST7789 通常使用 SPI。当前代码使用下面这组接线：

| ST7789 引脚 | 建议 ESP32-S3 引脚 | 说明 |
| --- | --- | --- |
| VCC | 3V3 | 多数 ST7789 模块用 3.3V |
| GND | GND | 共地 |
| SCL / SCK | GPIO21 | SPI 时钟 |
| SDA / MOSI | GPIO47 | SPI 数据 |
| CS | GPIO41 | 片选 |
| DC | GPIO40 | 数据/命令 |
| RST / RES | GPIO45 | 复位 |
| BLK / BL | GPIO42 | 背光 |

注意：不同 ST7789 模块的分辨率可能是 `240x240`、`240x280` 或 `240x320`，后面写屏幕代码时需要确认。

当前代码默认分辨率是 `240x240`。如果你的屏幕是 `240x280` 或 `240x320`，可以先修改 `main/main.c` 里的：

```c
#define LCD_H_RES 240
#define LCD_V_RES 240
```

如果屏幕有背光但文字位置偏移，可以调整：

```c
#define LCD_X_GAP 0
#define LCD_Y_GAP 0
```

如果颜色明显不对，可以尝试把这行里的 `true` 改成 `false`：

```c
ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_lcd_panel, true));
```

如果文字看起来像“缺少像素点”，可以分两种情况判断：

- 像素是随机缺失、边缘有花点：优先检查 SCL/SCK、SDA/MOSI、GND 是否接牢，杜邦线尽量短一些；当前代码已把 `LCD_PIXEL_CLOCK_HZ` 降到 `10 MHz`，比 `20 MHz` 更稳。
- 某个汉字完全不显示：说明它不在当前 LVGL 中文字库里，把这个字加入 `main/fonts/app_chinese_chars.txt` 后重新生成字体。

## 第 3 步：麦克风音量显示到屏幕

屏幕主界面会显示：

- `网络`：Wi-Fi 连接状态
- `服务`：OpenAI Realtime WebSocket 连接状态
- `麦克风`：根据本批采样峰值换算出的百分比，方便肉眼观察声音大小
- `等待识别结果 / 收到文字`：语音识别结果状态

麦克风百分比使用自动缩放：刚开始以 `VOLUME_INITIAL_FULL_SCALE` 作为满格参考值，如果拍手或说话出现更大的峰值，代码会自动把参考值调高。不同 INMP441 模块、接线长度、环境噪声都会影响数值，这是正常的。

如果你想让百分比更容易接近 100%，可以把 `main/app_config.h` 里的这个值调小：

```c
#define VOLUME_INITIAL_FULL_SCALE 2000
```

如果你想让百分比不要那么敏感，可以把它调大，例如改成 `5000`。

## 第 4 步：MAX98357A 扬声器

MAX98357A 也是 I2S 设备。建议先等麦克风和屏幕跑通后再接它。

后续可预留：

| MAX98357A 引脚 | 建议 ESP32-S3 引脚 | 说明 |
| --- | --- | --- |
| VIN | 5V 或 3V3 | 看模块说明，常见可接 5V |
| GND | GND | 共地 |
| BCLK | GPIO7 | I2S 位时钟 |
| LRC / WS | GPIO8 | I2S 左右声道时钟 |
| DIN | GPIO9 | 音频数据输入 |
| SD | 3V3 | 使能；有些模块可悬空 |

## 语音转文字该怎么做

ESP32-S3 比较适合做“离线固定命令识别”，例如：

- 你好
- 开灯
- 关灯
- 播放
- 停止

屏幕上显示这些识别结果是可行的。

当前项目已经接入云端实时语音转文字骨架。屏幕能显示字库内包含的中文；串口会打印完整识别结果。如果要让“任意中文句子”都能在屏幕完整显示，需要继续扩大 LVGL 中文字库或改用外部字体加载方案。

## 下一步

当前请先完成：

1. 按 README 接好 INMP441 和 ST7789
2. 烧录当前工程
3. 打开串口监视器
4. 观察屏幕是否先显示“启动中”，随后切换到语音转文字状态界面
5. 观察说话时 `volume avg` 和 `peak` 是否变大
6. 观察说话或拍手时屏幕上的 `麦克风` 百分比是否变化

只要屏幕麦克风百分比和串口数值都能变化，下一步就可以继续接 MAX98357A 扬声器，或继续完善语音转文字显示。

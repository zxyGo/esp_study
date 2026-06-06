# ESP32-S3 实时语音转文字（FunASR 本地版）

ESP32-S3 采集麦克风音频，通过 Wi-Fi 实时推送到**本地 FunASR WebSocket 服务器**，识别结果显示在 ST7789 屏幕上。**不依赖任何云端 API Key，局域网内完全离线运行。**

---

## 系统架构

```
INMP441 麦克风
     │ I2S PCM16 16 kHz
     ▼
ESP32-S3  ──── WebSocket ws://PC:10095 ────▶  FunASR 服务器（Docker）
     │                                              │ VAD + Paraformer 模型
     │                                              ▼
ST7789 屏幕  ◀──── 识别结果（中文文字）  ──────────
```

---

## 快速开始

### 第 0 步：启动 FunASR 服务端（PC 上）

> 需要已安装 Docker Desktop。首次运行会自动下载模型，约 2-4 GB，请耐心等待。

```powershell
cd e:\study\esp\FunASR\server
docker compose up -d
```

验证服务是否就绪（看到 `decoder thread ready` 字样即可）：

```powershell
docker compose logs -f
```

服务监听 `ws://0.0.0.0:10095`，**无需 TLS，无需鉴权**。

---

### 第 1 步：修改配置文件

打开 `main/app_config.h`，只需修改两处：

```c
/* Wi-Fi 配网热点密码（至少 8 位）——首次上电会开热点，手机连接后配网 */
#define WIFI_PORTAL_AP_PASS "12345678"

/* FunASR 服务端地址：把 IP 换成运行 Docker 的 PC 的局域网 IP */
#define FUNASR_WS_URL  "ws://192.168.1.100:10095"
```

查看本机 IP（PowerShell）：

```powershell
(Get-NetIPAddress -AddressFamily IPv4 -InterfaceAlias "WLAN*","以太网*" |
    Where-Object { $_.IPAddress -notmatch "^169" }
).IPAddress
```

---

### 第 2 步：接线

#### INMP441 麦克风

| INMP441 | ESP32-S3 | 说明 |
|---------|----------|------|
| VCC     | 3V3      | 不要接 5V |
| GND     | GND      | 共地 |
| SCK/BCLK | GPIO5   | I2S 位时钟 |
| WS/LRCLK | GPIO4   | I2S 左右声道时钟 |
| SD/DOUT  | GPIO6   | 麦克风数据输出 |
| L/R      | GND 或 3V3 | 选择声道；代码同时读取双声道，任意接均可 |

#### ST7789 屏幕（SPI）

| ST7789 | ESP32-S3 | 说明 |
|--------|----------|------|
| VCC    | 3V3      | |
| GND    | GND      | |
| SCL/SCK | GPIO21  | SPI 时钟 |
| SDA/MOSI | GPIO47 | SPI 数据 |
| CS     | GPIO41   | 片选 |
| DC     | GPIO40   | 数据/命令 |
| RST    | GPIO45   | 复位 |
| BLK/BL | GPIO42  | 背光 |

如果引脚不同，只需修改 `main/app_config.h` 顶部的宏定义。

---

### 第 3 步：编译和烧录

```powershell
# 进入 ESP-IDF 环境
cmd.exe /c "C:\esp\v5.4.4\esp-idf\export.bat && idf.py set-target esp32s3 && idf.py build"

# 烧录（把 COM4 改成实际端口）
cmd.exe /c "C:\esp\v5.4.4\esp-idf\export.bat && idf.py -p COM4 flash monitor"
```

退出串口监视器：`Ctrl + ]`

---

### 第 4 步：Wi-Fi 配网

首次上电后，ESP32-S3 会开启临时热点 `ESP32S3_STT`（密码：`12345678`），手机或电脑连接该热点后，打开浏览器访问 `192.168.4.1` 输入家庭 Wi-Fi 的 SSID 和密码即可完成配网。配网成功后热点自动关闭，下次开机直接连已保存的 Wi-Fi。

---

### 第 5 步：确认运行正常

屏幕会依次显示：

| 状态行 | 正常内容 |
|--------|---------|
| 网络   | `网络:已连接`（绿色）|
| 服务   | `服务:已连接`（绿色）|
| 麦克风 | `麦克风: xx%`，说话时数值变化 |
| 识别区 | 说话后显示识别出的中文句子 |

串口同时打印：

```
I (xxxx) APP_STT: [在线] 你好
I (xxxx) APP_STT: [离线] 你好，今天天气怎么样？
```

`[在线]` 是流式实时片段，`[离线]` 是 VAD 检测到静音后的完整句子。

---

## 代码结构

```text
esp32s3/
├── main/
│   ├── main.c              # 程序入口：初始化硬件→联网→启动 STT→循环发送音频
│   ├── app_config.h        # 所有可配置项（引脚、URL、采样率等）
│   ├── app_mic.c/.h        # INMP441 I2S 初始化，输出 16 kHz PCM16
│   ├── app_stt.c/.h        # FunASR WebSocket 客户端（2pass 流式协议）
│   ├── app_wifi.c/.h       # Wi-Fi 配网门户 + 重连
│   ├── app_lcd.c/.h        # ST7789 + LVGL 界面
│   ├── app_fonts.h         # LVGL 中文字体声明
│   ├── fonts/              # 自定义中文字符集和生成的字体 C 文件
│   ├── CMakeLists.txt
│   └── idf_component.yml   # 依赖：esp_websocket_client / esp_lvgl_port / lvgl
└── server/
    └── docker-compose.yml  # FunASR 服务端（一键 docker compose up -d）
```

---

## FunASR 协议说明

| 步骤 | 内容 |
|------|------|
| 连接 | `ws://server:10095`，无 TLS，无鉴权 |
| 握手 | 发送 JSON：`{"mode":"2pass","is_speaking":true,"wav_format":"pcm","audio_fs":16000,...}` |
| 音频 | 持续发送**原始二进制 PCM16**（每包 480 帧 = 30 ms） |
| 结果 | `mode:"2pass-online"` 实时片段；`mode:"2pass-offline"` VAD 断句后完整句子 |
| 断句 | 服务端 VAD 自动检测静音，客户端无需主动提交 |

---

## 常见问题

**麦克风百分比一直为 0**  
→ 检查 VCC 接 3V3、三根 I2S 线（BCLK/WS/DIN）是否接对 GPIO、杜邦线是否接牢。

**`服务:断开` 或一直显示`连接中`**  
→ 确认 Docker 服务已运行（`docker compose logs`）、ESP32 与 PC 在同一局域网、`FUNASR_WS_URL` 中 IP 填写正确。

**屏幕显示`服务:已连接`但没有识别结果**  
→ 对着麦克风说话后保持约 1 秒静音，等待 VAD 触发；串口监视器可看到原始日志辅助排查。

**某个汉字在屏幕上不显示**  
→ 把缺失的字加入 `main/fonts/app_chinese_chars.txt`，重新生成字体后编译：
```powershell
powershell.exe -ExecutionPolicy Bypass -File tools\gen_lvgl_font.ps1
```

---

## 硬件清单

| 器件 | 型号 |
|------|------|
| 主控 | ESP32-S3 开发板 |
| 麦克风 | INMP441 |
| 屏幕 | ST7789 240×240 |
| 音频功放（预留） | MAX98357A |

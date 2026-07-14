# ESP32-S3 本地语音助手（FunASR + 大模型）

ESP32-S3 采集麦克风音频，通过 Wi-Fi 实时推送到本地 FunASR；完整识别文本再交给大模型，回答显示在 ST7789 屏幕上。默认连接电脑上的 Ollama，可在局域网内完全离线运行；也可以通过兼容网关切换到云端 OpenAI Chat Completions 接口，API Key 只保存在电脑端，不会写入固件。

---

## 系统架构

```
INMP441 麦克风
     │ I2S PCM16 16 kHz
     ▼
ESP32-S3  ── WebSocket :10095 ──▶ FunASR（VAD + Paraformer）
     │                                  │ 完整识别文本
     │                                  ▼
     │      HTTP :10096          LLM Gateway（Docker）
     └──────────────────────────▶       │ OpenAI 兼容接口
                                        ▼
ST7789 屏幕 ◀───── 大模型回答 ───── Ollama / 云端模型
```

---

## 快速开始

### 第 0 步：准备大模型并启动服务端（PC 上）

> 需要已安装 Docker Desktop。首次运行会自动下载模型，约 2-4 GB，请耐心等待。

默认配置使用宿主机 Ollama。先确保 Ollama 已启动，并准备模型：

```powershell
ollama pull qwen2.5:3b
```

如果要改用其他 OpenAI 兼容服务，先创建环境文件：

```powershell
cd e:\study\esp\FunASR\server
Copy-Item .env.example .env
```

然后编辑 `.env` 中的 `LLM_API_BASE`、`LLM_MODEL` 和 `LLM_API_KEY`。使用本机 Ollama 时不需要 API Key。

```powershell
cd e:\study\esp\FunASR\server
docker compose up -d --build
```

验证服务是否就绪（FunASR 日志出现 `decoder thread ready`，两个容器均为运行状态）：

```powershell
docker compose ps
docker compose logs -f funasr llm-gateway
```

FunASR 监听 `ws://0.0.0.0:10095`，LLM 网关监听 `http://0.0.0.0:10096`；二者都只应暴露在可信局域网中。

当前 Compose 配置运行 FunASR 2pass 服务，并加载离线识别、在线识别、VAD、标点、ITN 和语言模型。下载的语音模型保存在 Docker 命名卷 `funasr-models` 中，重建容器后无需重复下载。LLM 网关保留最近 4 轮对话上下文，可以通过 `.env` 调整。

---

### 第 1 步：修改配置文件

打开 `esp32s3/main/app_config.h`，修改以下配置：

```c
/* Wi-Fi 配网热点密码（至少 8 位）——首次上电会开热点，手机连接后配网 */
#define WIFI_PORTAL_AP_PASS "12345678"

/* FunASR 服务端地址：把 IP 换成运行 Docker 的 PC 的局域网 IP */
#define FUNASR_WS_URL  "ws://192.168.1.100:10095"

/* LLM 网关使用同一台 PC 的局域网 IP */
#define LLM_GATEWAY_URL "http://192.168.1.100:10096/chat"
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
# 从仓库根目录进入固件工程
cd esp32s3

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
| 麦克风 | `麦克风: xx%`，按最近 300 ms 的峰值动态变化 |
| 主区域 | 先显示识别问题和`正在思考`，随后显示大模型回答 |

串口同时打印：

```
I (xxxx) APP_STT: [在线] 你好
I (xxxx) APP_STT: [离线] 你好，今天天气怎么样？
I (xxxx) APP_LLM: 提问: 你好，今天天气怎么样？
I (xxxx) APP_LLM: 回答: 我无法获取实时天气，但可以帮你分析天气信息。
```

`[在线]` 是流式实时片段，`[离线]` 是 VAD 检测到静音后的完整句子；只有完整句子会提交给大模型。模型请求在独立 FreeRTOS 任务中执行，不会阻塞麦克风采集和音频 WebSocket。

音量百分比使用会自动回落的参考峰值：较大的声音会动态抬高基准，随后逐步恢复，因此上电尖峰不会把音量条长期锁在 0%。有非零采样但幅度不足 1% 时会显示 1%，全零采样仍显示红色 0%。

---

## 代码结构

```text
.
├── esp32s3/
│   ├── main/
│   │   ├── main.c                    # 程序入口、音频发送和自适应音量显示
│   │   ├── app_config.h              # 所有可配置项（引脚、URL、采样率等）
│   │   ├── app_mic.c/.h              # INMP441 I2S 初始化，输出 16 kHz PCM16
│   │   ├── app_stt.c/.h              # FunASR WebSocket 客户端（2pass 流式协议）
│   │   ├── app_llm.c/.h              # 异步调用局域网 LLM 网关
│   │   ├── app_wifi.c/.h             # Wi-Fi 配网门户 + 重连
│   │   ├── app_lcd.c/.h              # ST7789 + LVGL 界面
│   │   ├── app_fonts.h               # LVGL 中文字体声明
│   │   ├── fonts/
│   │   │   ├── gb2312_level1.txt     # GB2312 一级常用汉字（3755 字）
│   │   │   ├── app_chinese_chars.txt # 生僻字和特殊符号扩展集
│   │   │   └── app_font_chinese_16.c # 生成的 16 px、2 bpp LVGL 字体
│   │   ├── CMakeLists.txt
│   │   └── idf_component.yml         # ESP-IDF 组件依赖
│   └── tools/
│       └── gen_lvgl_font.ps1         # 合并字符集并重新生成字体
└── server/
    ├── docker-compose.yml            # FunASR + LLM 网关编排
    ├── llm_gateway.py                # OpenAI 兼容网关与短期会话记忆
    ├── Dockerfile.llm
    └── .env.example                  # 模型服务配置示例
```

---

## 中文字体维护

默认字体由三部分组成：ASCII 可打印字符、`gb2312_level1.txt` 中的 3755 个一级常用汉字，以及 `app_chinese_chars.txt` 中的附加字符。普通识别结果通常无需再维护字符表。

如果需要显示 GB2312 一级字库之外的生僻字或特殊符号：

1. 将字符加入 `esp32s3/main/fonts/app_chinese_chars.txt`。
2. 确保已安装 Node.js（脚本通过 `npx` 调用 `lv_font_conv`），然后在固件工程目录重新配置依赖、生成字体并编译：

```powershell
cd esp32s3
cmd.exe /c "C:\esp\v5.4.4\esp-idf\export.bat && idf.py reconfigure"
powershell.exe -ExecutionPolicy Bypass -File tools\gen_lvgl_font.ps1
cmd.exe /c "C:\esp\v5.4.4\esp-idf\export.bat && idf.py build"
```

`reconfigure` 会准备 LVGL 自带的 `SimSun.woff`；字体脚本会校验 GB2312 一级字库必须正好包含 3755 个字符，并生成 `main/fonts/app_font_chinese_16.c`。首次运行 `npx` 时可能需要联网下载 `lv_font_conv`。该生成文件体积较大，不建议手工编辑。

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

## 大模型网关接口

ESP32 向 `POST /chat` 发送：

```json
{"text":"你好，请介绍一下自己","session_id":"esp32s3-voice-assistant"}
```

成功时网关返回：

```json
{"answer":"你好，我是你的本地语音助手。"}
```

网关再调用上游的 `/v1/chat/completions`。`LLM_SYSTEM_PROMPT`、最大输出 token、温度和历史轮数均可在 `server/.env` 中配置。使用默认开启思考模式的模型（如智谱 GLM-4.7-Flash）进行短语音问答时，可设置 `LLM_THINKING=disabled`，防止输出 token 被思考过程耗尽而返回空回答。

---

## 常见问题

**麦克风百分比一直为 0**  
→ 检查 VCC 接 3V3、三根 I2S 线（BCLK/WS/DIN）是否接对 GPIO、杜邦线是否接牢；串口中若持续出现 `I2S 读取失败`，也应优先检查接线和引脚配置。

**`服务:断开` 或一直显示`连接中`**  
→ 确认 Docker 服务已运行（`docker compose logs`）、ESP32 与 PC 在同一局域网、`FUNASR_WS_URL` 中 IP 填写正确。

**屏幕显示`服务:已连接`但没有识别结果**  
→ 对着麦克风说话后保持约 1 秒静音，等待 VAD 触发；串口监视器可看到原始日志辅助排查。

**屏幕显示`模型请求失败`**
→ 先运行 `docker compose logs llm-gateway`。使用 Ollama 时确认 Ollama 已启动、模型已拉取，并且宿主机的 `11434` 端口允许 Docker 访问；使用云端模型时检查 `.env` 中的接口地址、模型名和 API Key。也可以在 PC 上访问 `http://127.0.0.1:10096/health` 检查网关本身。

**某个汉字在屏幕上不显示**  
→ 默认字体已覆盖 GB2312 一级常用汉字。对于未覆盖的生僻字，把它加入 `esp32s3/main/fonts/app_chinese_chars.txt`，重新生成字体后编译：
```powershell
cd esp32s3
powershell.exe -ExecutionPolicy Bypass -File tools\gen_lvgl_font.ps1
cmd.exe /c "C:\esp\v5.4.4\esp-idf\export.bat && idf.py build"
```

---

## 硬件清单

| 器件 | 型号 |
|------|------|
| 主控 | ESP32-S3 开发板 |
| 麦克风 | INMP441 |
| 屏幕 | ST7789 240×240 |
| 音频功放（预留） | MAX98357A |

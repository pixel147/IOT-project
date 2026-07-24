# CLAUDE.md — 情绪守护项目

## 项目概述

基于 ESP32-P4-Function-EV-Board 的情绪识别 + 大模型聊天终端。
CSI 摄像头实时捕获 → 人脸检测 → 情绪分类 → 大模型聊天。

**目标**: 情绪监管与陪伴聊天
**框架**: ESP-IDF v5.5.4, LVGL v9.5.0 (`esp_lvgl_port` v2.8.0)
**硬件**: ESP32-P4-Function-EV-Board, DSI LCD (EK79007), MIPI-CSI 摄像头 (SC2336/OV5647)

---

## 当前架构 (2026-07-24)

```
Camera → CSI V4L2 → 50fps RGB565
  │
  ├─ Camera Callback (Core 1, 每帧)
  │    ├─ memcpy → s_fb_disp (显示缓冲)
  │    ├─ [llm_is_busy() 时跳过] face_detect_run() [ESP-DL, ~26ms]
  │    ├─ draw_rect_rgb565() + ui_update_emotion()
  │    ├─ ui_chat_update_emotion()  ← 聊天页情绪徽章同步
  │    ├─ taskYIELD()  ← 防止 TWDT 超时
  │    └─ lv_refr_now()
  │
  ├─ emotion_task (Core 0, 每2秒)
  │    ├─ memcpy face ROI → roi_buf
  │    ├─ emotion_espdl_run() [ESP-DL INT8, ~2-8ms]
  │    ├─ update s_ai_classes[] (供相机回调读取)
  │    └─ emotion_provider_cb [Pull] 每次 chat 前实时拉取情绪注入大模型
  │
  └─ WiFi + LLM 模块
       ├─ wifi_init() → STA idle（扫描可用，不自动连接）
       ├─ wifi_connect(NULL) → NVS 凭据自动连接（非阻塞）
       ├─ on_wifi_status(CONNECTED)
       │    ├─ wifi_ping_test("bilibili.com") → 连通性验证
       │    ├─ 10s 延迟 ping → 网络质量检查
       │    ├─ llm_init() + llm_set_emotion_provider()
       │    └─ slave_ota_perform() → C6 固件版本检查/OTA
       └─ llm_chat_async() → FreeRTOS 任务 (16KB 栈, 响应堆分配)
             └─ HTTPS → DeepSeek API（超时 15s）
```

### 多屏幕架构（4 屏）

```
启动 → Home 屏幕 (lv_screen_load)
         ├─ [📷 情绪监控] → Monitor 屏幕 (相机+人脸+情绪)
         ├─ [✉ AI 聊天]  → Chat 屏幕 (LLM 对话 + 情绪徽章)
         └─ [⚙ 设置]     → Settings 屏幕 (WiFi 扫描/连接/管理)
```

---

## 关键文件

| 文件 | 说明 |
|------|------|
| `main/app_main.c` | 入口: 显示初始化, 三缓冲, 相机回调, 人脸检测(LLM期间暂停), 情绪任务, WiFi状态回调, C6 OTA |
| `main/ui/ui.c` | Monitor 屏幕: 暗色主题, 三栏布局, 情绪徽章, 图例, 信息面板, 建议栏 |
| `main/ui/chat.c` | Chat 屏幕: LLM 对话界面, LVGL 键盘, 情绪徽章(实时同步摄像头), 对话历史 |
| `main/ui/settings.c` | Settings 屏幕: WiFi 扫描(可点击), 密码对话框+键盘, NVS 保存, 忘记网络 |
| `main/ui/home.c` | Home 屏幕: App 图标网格, 点击切换 screen |
| `main/ui_font_zh_22.c` | 22px 中文字体 (510 字 + 标点, 由 `tools/generate_ui_font.py` 生成) |
| `main/camera/camera.c` | CSI V4L2 摄像头驱动 (MIPI-CSI RGB565, MMAP 三缓冲, taskYIELD) |
| `main/wifi/wifi.c` | WiFi: NVS 凭据, 非阻塞连接, 状态机+回调, 连通性测试(ping) |
| `main/wifi/slave_ota.c` | C6 OTA: 版本检查, 固件推送, 自动跳过已最新 |
| `main/llm/llm_client.c` | LLM: 情绪感知系统提示词, 异步聊天(16KB栈+堆响应), Pull 模式情绪注入 |
| `main/llm/llm_config.h` | LLM 配置: Base URL 预设, 情绪提示词数组 |
| `main/model_loader/face_detect_wrapper.cpp` | ESP-DL 人脸检测 (HumanFaceDetect MSRMNP_S8_V1) |
| `main/model_loader/emotion_espdl.cpp` | ESP-DL 情绪识别 INT8 (48×48灰度, 7类, ~2-8ms) |

### 已删除

| 文件 | 原因 |
|------|------|
| `main/model_loader/emotion_tflite.*` | TFLite 替换为 ESP-DL |
| `main/wifi/wifi_credentials.h` | 凭据改为 NVS 运行时配置 |

---

## WiFi 模块 (`main/wifi/`)

### 设计

凭据通过 NVS 持久化（namespace `"wifi_creds"`, key `"ssid"` / `"password"`）。
启动时 `wifi_init()` 开启 STA idle 模式（扫描可用），`wifi_connect(NULL)` 尝试 NVS 自动连接。

### API

```c
// 生命周期
esp_err_t wifi_init(void);
esp_err_t wifi_connect(const char *ssid, const char *password); // NULL=从NVS加载
void      wifi_disconnect(void);

// 状态 + 回调
typedef enum { WIFI_STATUS_DISCONNECTED, WIFI_STATUS_CONNECTING,
               WIFI_STATUS_CONNECTED, WIFI_STATUS_ERROR } wifi_status_t;
wifi_status_t wifi_get_status(void);
bool wifi_is_connected(void);
void wifi_set_status_callback(wifi_status_cb_t cb);

// NVS 凭据
esp_err_t wifi_save_credentials(const char *ssid, const char *password);
bool      wifi_has_saved_credentials(void);
esp_err_t wifi_get_saved_ssid(char *buf, size_t size);
esp_err_t wifi_clear_credentials(void);

// 连通性测试
esp_err_t wifi_ping_test(int *latency_ms);  // DNS 解析 bilibili.com
```

### C6 协处理器

ESP32-P4 无原生 WiFi，通过 ESP32-C6 协处理器 (SDIO Slot 1) 提供。
- 组件: `espressif/esp_wifi_remote: 0.13.*` + `espressif/esp_hosted: 2.11.*`
- C6 固件: v2.11.7（通过 OTA 从 `slave_fw` 分区推送）
- C6 slave 固件源码: `managed_components/espressif__esp_hosted/slave/`
- 构建: `D:/Code_Projects/IOT/slave_fw/` → `idf.py set-target esp32c6 build`
- 烧录: `slave_fw.bin` @ flash 0x610000

---

## LLM 模块 (`main/llm/`)

### 架构

```
llm_client.c (封装层)
  ├─ 情绪感知系统提示词 → rebuild_system_prompt() → chat->setSystem()
  ├─ Pull 模式: llm_set_emotion_provider(cb) → 每次 chat 前拉取最新情绪
  ├─ 情绪查询: llm_get_last_emotion() → 供 UI 徽章显示
  ├─ 异步: llm_chat_async() → xTaskCreate("llm_async", 16KB, prio=2)
  │    └─ 响应缓冲堆分配(4KB) + HTTPS → 回调
  └─ 互斥锁线程安全, busy 标志防重入
      ↓
espressif/openai (官方组件)
  ├─ OpenAICreate → chatCreate → multiModalMessage
  ├─ TLS: esp_crt_bundle_attach
  ├─ HTTP 超时: 15s (修改自默认 60s)
  └─ 对话历史: chat->message(msg, save=true)
```

### 情绪感知机制

7 种情绪 (class 0-6) 各有专属引导提示词 (`LLM_EMOTION_PROMPTS[]`)：

| 情绪 | 提示词语气 |
|------|-----------|
| 生气 | 温和安抚，帮 TA 平复情绪 |
| 厌恶 | 理解感受，引导放宽心态 |
| 害怕 | 给安全感 + 鼓励 |
| 开心 | 分享快乐，轻松愉快 |
| 难过 | 温暖安慰 + 情感支持 |
| 惊讶 | 探索惊喜，有趣互动 |
| 平静 | 自然轻松的日常陪伴 |

系统提示词拼接: `[基础提示词] + [情绪感知] 难过（置信度 88%）+ 专属引导`

### 已知限制

- `espressif/openai` HTTP 超时硬编码 60s → 手动改为 15s (`managed_components/espressif__openai/OpenAI.c`)
- 异步请求仅单并发 (`s_ctx.busy` 标志)
- LLM 请求期间暂停人脸检测（`llm_is_busy()` → 跳过），防止 PSRAM 并发冲突

---

## 字体

### 当前状态

- 工具: `tools/generate_ui_font.py` (Pillow, 从 `C:\Windows\Fonts\Deng.ttf` 生成)
- 格式: 4bpp 位图 + LVGL `FORMAT0_TINY` cmap
- 字数: **510 字 + ASCII + 标点** (LVGL cmap 9-bit 最大 511 条)
- 生成: `python tools/generate_ui_font.py`

### LVGL cmap 限制

`lv_font_fmt_txt_dsc_t.cmap_num` 为 9-bit 字段，最大 511 个 cmap 条目。
字符数超过会导致溢出（683 → 171 截断错误）。当前精确控制在 510 以内。

### 未来方案

字体放 SD 卡/Flash mmap 可突破 510 字限制 → 覆盖 3000+ 汉字。

---

## 模型

| 功能 | 框架 | 模型路径 (SD卡) | 延迟 |
|------|------|----------------|:----:|
| 人脸检测 | ESP-DL | `/sdcard/models/human_face_detect_msr_s8_v1.espdl` + `mnp_s8_v1.espdl` | ~26ms |
| 情绪识别 | ESP-DL INT8 | `/sdcard/models/emotion_cnn_aug_pt.espdl` (101KB) | ~2-8ms |
| 大模型聊天 | espressif/openai | HTTPS API (DeepSeek), 超时 15s | 2-5s |

---

## 内存布局

| 缓冲 | 位置 | 大小 |
|------|------|:----:|
| `s_fb_cap[2]` (乒乓捕获) | PSRAM | 2 × 614KB |
| `s_fb_disp` (显示) | PSRAM | 614KB |
| `roi_buf` (情绪任务) | PSRAM | 614KB |
| ESP-DL 模型权重 | PSRAM | ~500KB |
| 人脸检测模型数据 | PSRAM | ~1MB |
| 字体 (编译进固件) | PSRAM | ~700KB |
| LLM 异步响应缓冲 | PSRAM 堆 | 4KB |
| LLM 异步任务栈 | SRAM/PSRAM | 16KB |
| 内部 SRAM 总计 | — | ~551KB |

---

## 构建命令

```bash
# 完整构建
idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.esp32_p4_function_ev_board build

# 构建+烧录+监视
idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.esp32_p4_function_ev_board -p COM7 build flash monitor

# 仅烧录 app (不擦除 slave_fw)
idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.esp32_p4_function_ev_board -p COM7 flash

# 字体重新生成
python tools/generate_ui_font.py

# C6 slave 固件构建 (位于 D:/Code_Projects/IOT/slave_fw/)
cd D:/Code_Projects/IOT/slave_fw && idf.py set-target esp32c6 && idf.py build
# 然后复制 network_adapter.bin 到项目根目录，烧录：
python -m esptool --chip esp32p4 -p COM7 -b 460800 --force \
  write_flash 0x610000 slave_fw.bin
```

---

## C6 固件 OTA 机制

### 流程

```
启动 → wifi_init() → SDIO 链路建立
  → slave_ota_perform()
    ├─ 读取 slave_fw 分区 (0x610000, 2MB)
    ├─ 解析 ESP 固件头 → 计算精确大小
    ├─ esp_hosted_get_coprocessor_fwversion() → 检查 C6 当前版本
    ├─ 已是最新 (≥ 2.11.7) → 跳过
    └─ 需要更新 → esp_hosted_slave_ota_begin/write/end → C6 自更新
```

### 分区表

```
# Name      Type SubType Offset    Size
nvs         data nvs     0x9000    24K
phy_init    data phy     0xf000     4K
factory     app  factory 0x10000    6M
slave_fw    data 0x40    0x610000   2M   ← C6 固件存储
storage     data spiffs  0x810000   2M
```

---

## 已知问题

### 已修复 (2026-07-24)

- ~~TFLite Micro → ESP-DL PER_TENSOR，推理 <10ms~~
- ~~TFLite 文件全部清除~~
- ~~WiFi 凭据硬编码 → NVS 持久化 + 非阻塞启动 + 设置页交互~~
- ~~C6 固件 v0.0.0 → v2.11.7 (OTA 推送)~~
- ~~Chat 页情绪徽章始终"平静0%" → 摄像头回调实时同步~~
- ~~LLM 请求导致 TWDT 崩溃 → `llm_is_busy()` 期间跳过人脸检测~~
- ~~LLM 异步任务栈 8KB → 16KB + 响应缓冲堆分配~~
- ~~摄像头 TWDT 超时 → `taskYIELD()` + 帧缓冲 cache sync~~
- ~~HTTP 超时 60s → 15s (OpenAI.c)~~
- ~~字体覆盖不足 → 510 字 + ASCII 标点 + 中文标点~~

### 待解决

- WiFi 间歇断连 (reason=2/201/205)，根在 C6 侧 SDIO/WiFi 稳定性
- LLM 响应速度取决于网络 (ping bilibili ~443ms)
- 多人脸只处理第一张
- ISP 色彩未校准，相机画面偏绿
- 字体 510 字限制 (SD卡/flash mmap 可突破)

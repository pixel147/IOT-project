# CLAUDE.md — 情绪守护项目

## 项目概述

基于 ESP32-P4-Function-EV-Board 的情绪识别 + 大模型聊天终端。
CSI 摄像头实时捕获 → 人脸检测 → 情绪分类 → 大模型聊天。

**目标**: 情绪监管与陪伴聊天
**框架**: ESP-IDF v5.5.4, LVGL v9.5.0 (`esp_lvgl_port` v2.8.0)
**硬件**: ESP32-P4-Function-EV-Board, DSI LCD (EK79007), MIPI-CSI 摄像头 (SC2336/OV5647)

---

## 当前架构 (2026-07-23)

```
Camera → CSI V4L2 → 30fps RGB565
  │
  ├─ Camera Callback (Core 1, 30fps)
  │    ├─ memcpy → s_fb_disp (显示缓冲)
  │    ├─ face_detect_run() [ESP-DL, ~32ms]
  │    ├─ draw_rect_rgb565() + ui_update_emotion()
  │    └─ lv_refr_now()
  │
  ├─ emotion_task (Core 0, 每2秒)
  │    ├─ memcpy face ROI → roi_buf
  │    ├─ emotion_tflite_run() [TFLite Micro INT8, ~130ms]
  │    ├─ update s_ai_classes[] (供相机回调读取)
  │    └─ emotion_provider_cb [Pull] 每次 chat 前实时拉取情绪注入大模型
  │
  └─ [NEW] WiFi + LLM 模块
       ├─ wifi_connect() → 连接无线路由器
       ├─ llm_init() → 初始化 espressif/openai 组件
       └─ llm_chat() → 发送消息 / 接收回复（带情绪上下文）
```

### 状态栏 → 左图例 + 相机 + 右信息 → 建议栏 → 控制栏

```
┌──────────────────────────────────────────┐
│ 情绪守护          启动中          30 FPS │ 44px
├──────┬────────────────────┬──────────────┤
│ 生气 │                    │    平静       │
│ 厌恶 │   相机预览          │    92%       │
│ 害怕 │  (比例缩放填充)     │  ─────────   │
│ 开心 │                    │ 享受此刻宁静  │
│ 难过 │   [😐 平静 92%]     │    0 min     │
│ 惊讶 │   (左上角徽章)      │              │
│ 平静 │                    │              │
├──────┴────────────────────┴──────────────┤
│ 💡 提示文字（自动滚动）                    │ auto
├──────────────────────────────────────────┤
│      [监控中]                  [⚙️]      │ 52px
└──────────────────────────────────────────┘
```

### 多屏幕架构 (2026-07-24)

项目改为 App 桌面风格，3 个 LVGL 独立 screen：

```
启动 → Home 屏幕 (lv_screen_load)
         ├─ [📷 情绪监控] → Monitor 屏幕 (相机+人脸+情绪)
         ├─ [✉ AI 聊天]  → 占位弹窗 "即将上线"
         └─ [⚙ 设置]     → Settings 屏幕 (WiFi 扫描列表)
```

所有屏幕在 `app_main` 初始化阶段一次性创建，通过 `lv_screen_load()` 切换，不删除旧屏幕。相机/情绪任务常驻运行，仅在切屏时切换 UI 可见性。

## 关键文件

| 文件 | 说明 |
|------|------|
| `main/app_main.c` | 入口: 显示初始化, 三缓冲, 相机回调, 人脸检测, 情绪任务, 监控按钮控制 |
| `main/ui/ui.c` | LVGL UI: 暗色主题, 三栏布局, 情绪徽章(左上角), 图例(左竖排), 信息面板(右), 建议栏 |
| `main/ui/ui.h` | UI 公共 API (含 `ui_update_emotion()`, `ui_hide_emotion()`, `ui_is_monitoring()`) |
| `main/ui/home.c` | **[NEW]** Home 屏幕: App 图标网格 (LV_SYMBOL + 色块), 点击切换 screen |
| `main/ui/home.h` | Home 屏幕 API: `ui_home_create()`, `ui_home_set_icon_callback()` |
| `main/ui/settings.c` | **[NEW]** Settings 屏幕: WiFi 扫描列表 (异步 FreeRTOS 任务 + LVGL 列表) |
| `main/ui/settings.h` | Settings 屏幕 API: `ui_settings_create()`, `ui_settings_scan_wifi()` |
| `main/ui_font_zh_22.c` | 22px 中文字体 (220 字, 由 `tools/generate_ui_font.py` 从 Deng.ttf 生成) |
| `main/camera/camera.c` | CSI V4L2 摄像头驱动 (MIPI-CSI RGB565, MMAP 三缓冲) |
| `main/sdcard/sdcard_init.cpp` | SDMMC 挂载到 `/sdcard` |
| `main/model_loader/face_detect_wrapper.cpp` | ESP-DL 人脸检测 (HumanFaceDetect MSRMNP_S8_V1) |
| `main/model_loader/emotion_tflite.cpp` | TFLite Micro 情绪识别 INT8 (48×48 灰度, 7 类, ~130ms) |
| `main/wifi/wifi.h` | **[NEW]** WiFi Station API (`wifi_connect`, `wifi_wait_connected`, `wifi_is_connected`) |
| `main/wifi/wifi.c` | **[NEW]** WiFi 实现: netif 初始化, 事件组同步, 自动重连 |
| `main/llm/llm_config.h` | **[NEW]** 大模型配置: Base URL 预设 (OpenAI/DeepSeek/Qwen/Groq/…), 模型名预设, 情绪提示词声明 |
| `main/llm/llm_client.h` | **[NEW]** 大模型客户端 API (`llm_init`, `llm_chat`, `llm_chat_async`, `llm_set_emotion_provider`) |
| `main/llm/llm_client.c` | **[NEW]** 大模型客户端实现: 封装 `espressif/openai` 组件, 情绪感知系统提示词, 异步支持 |

### 遗留文件（不参与编译）

| 文件 | 说明 |
|------|------|
| `main/model_loader/emotion_estimator.cpp` | 旧的 ESP-DL 情绪模型实现 (PER_CHANNEL 量化不兼容 → 改为 TFLite) |
| `main/model_loader/emotion_estimator.hpp` | 旧情绪模型头文件 |
| `main/legacy/lvgl_demo_ui.c` | 早期 LVGL demo |

---

## WiFi 模块 (`main/wifi/`)

### API

```c
esp_err_t wifi_connect(const char *ssid, const char *password);
esp_err_t wifi_wait_connected(uint32_t timeout_ms);   // 0 = 无限等待
bool      wifi_is_connected(void);
esp_err_t wifi_get_ip(char *buf, size_t size);
void      wifi_disconnect(void);
```

### 使用模式

```c
// 非阻塞连接 → 阻塞等待 IP → 就绪
wifi_connect("MyWiFi", "password");
if (wifi_wait_connected(15000) == ESP_OK) {
    ESP_LOGI(TAG, "WiFi ready");
    // 此时可以使用 llm_client
} else {
    ESP_LOGE(TAG, "WiFi timeout");
}
```

### 实现细节

- 内部一次性完成 `nvs_flash_init` / `esp_netif_init` / `esp_event_loop_create_default`
- 使用 FreeRTOS 事件组 `WIFI_CONNECTED_BIT` 同步连接状态
- 断开自动重连 (`WIFI_EVENT_STA_DISCONNECTED` → `esp_wifi_connect()`)
- `wifi_connect()` 幂等：已连接直接返回 `ESP_OK`
- WiFi 密码为空时自动设为 `WIFI_AUTH_OPEN`（开放网络）

---

## 大模型模块 (`main/llm/`)

### 架构

```
llm_client.c (薄封装, ~290行)
  │
  ├─ 情绪感知系统提示词 → chat->setSystem()
  ├─ 同步/异步聊天 API
  ├─ 互斥锁线程安全
  │
  ▼
espressif/openai (官方组件, ~2700行, idf_component.yml 已引入)
  │
  ├─ OpenAICreate(api_key) → OpenAIChangeBaseURL(url) → chatCreate()
  ├─ TLS: esp_crt_bundle_attach (正确证书验证, 非跳过)
  ├─ HTTP: esp_http_client POST
  ├─ JSON: cJSON 构建/解析
  ├─ 对话历史: chat->message(msg, save=true) 自动保存
  ├─ 错误处理: resp->getError(resp)
  ├─ 多模态: multiModalMessage (text / image_url / input_audio)
  └─ 其他: Embedding, Moderation, Image Generation, Audio Speech/Transcription
```

### API

```c
// === 生命周期 ===
esp_err_t llm_init(const llm_config_t *config);
void      llm_deinit(void);

// === 情绪上下文（核心增值 — Pull 模式） ===
/** 情绪提供者回调：LLM 在每次 chat 前调用，获取最新情绪 */
typedef void (*llm_emotion_provider_t)(int *emotion_class, float *confidence);
/** 注册情绪提供者（传 NULL 取消情绪感知） */
void llm_set_emotion_provider(llm_emotion_provider_t provider);
void llm_set_system_prompt(const char *prompt);  // 手动覆盖, NULL 恢复默认

// === 同步聊天（阻塞调用线程） ===
esp_err_t llm_chat(const char *user_message,
                   char *response_buf, size_t buf_size);
esp_err_t llm_chat_ex(const char *user_message,
                      char *response_buf, size_t buf_size,
                      int *out_http_code);

// === 异步聊天（FreeRTOS 任务, 结果回调） ===
esp_err_t llm_chat_async(const char *user_message,
                         llm_chat_callback_t callback, void *user_data);

// === 对话历史 ===
void llm_clear_history(void);
int  llm_get_turn_count(void);

// === 状态 ===
bool llm_is_busy(void);
```

### 配置结构体

```c
typedef struct {
    const char *base_url;    // Base URL（尾部带 /），组件自动追加 chat/completions
    const char *api_key;     // Bearer Token
    const char *model;       // 模型名称
    int         max_tokens;  // 最大生成 token 数 (0=默认256)
    float       temperature; // 采样温度 (0=默认0.8)
    int         timeout_ms;  // 超时毫秒 (0=默认30000)
} llm_config_t;
```

### 预设 Base URL 和模型

| 服务商 | Base URL 宏 | 模型宏 |
|--------|-----------|--------|
| OpenAI | `LLM_BASE_OPENAI` | `LLM_MODEL_GPT4O` / `LLM_MODEL_GPT4O_MINI` |
| DeepSeek | `LLM_BASE_DEEPSEEK` | `LLM_MODEL_DEEPSEEK_CHAT` / `LLM_MODEL_DEEPSEEK_REASONER` |
| 通义千问 | `LLM_BASE_QWEN` | `LLM_MODEL_QWEN_MAX` / `LLM_MODEL_QWEN_TURBO` |
| Groq | `LLM_BASE_GROQ` | `LLM_MODEL_GROQ_LLAMA4` |
| Together | `LLM_BASE_TOGETHER` | 自定义 |
| 本地 Ollama | `LLM_BASE_OLLAMA` | 自定义 (HTTP 明文, 不需要 TLS) |

### 情绪感知机制（Pull 模式）

不再由 emotion_task 每 2 秒主动 push 情绪到 LLM 模块。改为注册回调，LLM 每次 `llm_chat()` 前实时拉取最新情绪，保证单一数据源、零延迟：

```
emotion_task (每2秒)
    └─ s_ai_classes[0] = class    // 只维护这一份原始数据
       s_ai_confs[0]   = conf

llm_chat("你今天感觉怎么样？")
    ├─ rebuild_system_prompt()
    │     └─ 回调 → s_ai_classes[0]  ← 实时拉取，零延迟
    ├─ chat->setSystem(chat, …)     // 注入新鲜情绪
    └─ chat->message(…)
```

拼接效果：

```
[基础系统提示词]
"你是一个温暖、善解人意的情绪陪伴助手，名字叫「小守」…"

    ↓ 自动拼接

"你是一个温暖、善解人意的情绪陪伴助手…
 [情绪感知] 难过（置信度 88%）
 用户此刻有些难过。请用温暖的话语安慰 TA，给 TA 情感支持。"
```

7 种情绪（class 0-6）各有专属引导提示词，定义在 `llm_client.c` 的 `LLM_EMOTION_PROMPTS[]` 数组中。

### 最小接入示例

```c
#include "wifi.h"
#include "llm_client.h"

// 1. WiFi
wifi_connect("SSID", "PASSWORD");
wifi_wait_connected(15000);

// 2. LLM 初始化
llm_config_t cfg = {
    .base_url = LLM_BASE_DEEPSEEK,
    .api_key  = "sk-xxxxxxxxxxxxxxxx",
    .model    = LLM_MODEL_DEEPSEEK_CHAT,
};
llm_init(&cfg);

// 3. 注册情绪提供者（Pull 模式：每次 chat 前实时拉取）
llm_set_emotion_provider(my_emotion_provider);

// 4. 聊天（自动带上最新情绪上下文）
char reply[512];
llm_chat("我今天心情不太好", reply, sizeof(reply));
ESP_LOGI("CHAT", "小守: %s", reply);

// 5. 异步聊天（不阻塞）
llm_chat_async("讲个笑话", my_callback, NULL);
```

### 线程安全

- 所有公开 API 通过 FreeRTOS 互斥锁 (`s_ctx.mutex`) 保护共享状态
- 可在 `emotion_task` (Core 0) 和 UI 事件回调 (Core 1) 中安全调用
- 异步聊天使用独立 FreeRTOS 任务 (`llm_async`, 优先级 2, 栈 8KB)
- 同一时间只允许一个异步请求（`s_ctx.busy` 标志位）

---

## 模型

| 功能 | 框架 | 模型路径 (SD卡) | 延迟 |
|------|------|----------------|:----:|
| 人脸检测 | ESP-DL | `/sdcard/models/human_face_detect_msr_s8_v1.espdl` + `mnp_s8_v1.espdl` | ~32ms |
| 情绪识别 | TFLite Micro INT8 | `/sdcard/models/emotion_cnn_aug_int8.tflite` (95KB) | ~130ms |
| 大模型聊天 | espressif/openai | HTTPS API 调用, 延迟取决于网络和服务商 | 1-5s |

### 情绪映射

| 编号 | 中文 | 颜色 | 贴士 |
|:---:|:----:|:----:|------|
| 0 | 生气 | `#E53935` 红 | 深呼吸冷静一下 |
| 1 | 厌恶 | `#2E7D32` 深绿 | 试着放宽心吧 |
| 2 | 害怕 | `#1565C0` 蓝 | 别怕你很安全 |
| 3 | 开心 | `#43A047` 绿 | 保持好心情 |
| 4 | 难过 | `#E53935` 红 | 想点开心的事 |
| 5 | 惊讶 | `#FDD835` 黄 | 哇真惊喜呢 |
| 6 | 平静 | `#78909C` 灰 | 享受此刻宁静 |

## 构建命令

```bash
# 完整构建
idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.esp32_p4_function_ev_board build

# 构建+烧录+监视
idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.esp32_p4_function_ev_board -p COM7 build flash monitor

# 字体重新生成
python tools/generate_ui_font.py
```

SDKCONFIG 使用 `sdkconfig.bsp.esp32_p4_function_ev_board` 作为 board 覆盖配置。

## 内存布局

| 缓冲 | 位置 | 大小 |
|------|------|:----:|
| `s_fb_cap[2]` (乒乓捕获) | PSRAM, 64B对齐 | 2 × 614KB |
| `s_fb_disp` (显示) | PSRAM, 64B对齐 | 614KB |
| `roi_buf` (情绪任务) | PSRAM, 32B对齐 | 614KB |
| TFLite 张量竞技场 | PSRAM | 512KB |
| 人脸检测模型数据 | PSRAM | ~1MB |
| LLM HTTP 响应缓冲 | 栈 (llm_chat) | 4KB |
| 异步 LLM 任务栈 | FreeRTOS | 8KB |

### 三缓冲防撕裂
```
Camera DMA → s_fb_cap[0] ← 乒乓 → s_fb_cap[1]
                                      ↓ LVGL lock
                                 s_fb_disp → canvas → lv_refr_now()
```

## 关键依赖

| 组件 | 用途 |
|------|------|
| `esp32_p4_function_ev_board` | BSP (LCD, CSI, SD) |
| `esp-tflite-micro` | 情绪推理 (INT8) |
| `esp_lvgl_port` + `lvgl/lvgl` | LVGL |
| `esp_video` | ISP 视频管线 |
| `esp_lcd_ek79007` | DSI LCD |
| `espressif/openai` | **[NEW]** OpenAI 兼容大模型客户端 (ChatCompletion + TLS + JSON) |
| `esp_http_client` + `json` | (由 `espressif/openai` 间接依赖) |
| `human_face_detect` | 人脸检测 ESP-DL |

### WiFi 模块额外依赖 (接入时需添加到 CMakeLists REQUIRES)

| 组件 | 用途 |
|------|------|
| `nvs_flash` | WiFi 配置存储 |
| `esp_wifi` | WiFi 驱动 |
| `esp_netif` | TCP/IP 网络接口 |
| `esp_event` | 事件循环 (WiFi/IP 事件) |

## CMakeLists.txt 接入说明（已接入）

WiFi 和 LLM 模块已接入 `main/CMakeLists.txt` 和 `app_main.c`。当前配置：

```cmake
idf_component_register(SRCS "app_main.c" "ui/ui.c" "ui_font_zh_22.c" "camera/camera.c"
                       "sdcard/sdcard_init.cpp"
                       "model_loader/face_detect_wrapper.cpp"
                       "model_loader/emotion_tflite.cpp"
                       "wifi/wifi.c"                    # ✓ 已接入
                       "llm/llm_client.c"               # ✓ 已接入
                       INCLUDE_DIRS "." "ui" "camera" "sdcard" "model_loader" "wifi" "llm"
                       REQUIRES esp_video esp32_p4_function_ev_board esp_lvgl_port lvgl
                                human_face_detect esp-tflite-micro
                       PRIV_REQUIRES esp_timer esp-dl nvs_flash esp_wifi esp_netif esp_event)
```

注意：`espressif/openai` 已在 `main/idf_component.yml` 声明为依赖，不需要在 `REQUIRES` 中额外添加。

## 开发注意事项

### 字体修改
1. 修改 `tools/generate_ui_font.py` 的 `TEXT` 字串
2. `python tools/generate_ui_font.py` 重新生成
3. 提交 `main/ui_font_zh_22.c` + `.h`
4. **不要**在 `main/ui/` 下留有副本 (已清理)

### 跨核数据竞争
AI 任务(Core 0)和相机回调(Core 1)共享数据时必须:
- 读之前: `esp_cache_msync(INVALIDATE)` 刷新缓存
- 写之后: `esp_cache_msync(TYPE_DATA)` 写回
- 状态标志: `__sync_synchronize()` 内存屏障 + `volatile`

### 情绪模型
- `emotion_cnn_aug.espdl` (ESP-DL) 用 PER_CHANNEL 量化, 当前 ESP-DL `libfbs_model.a` 只支持 PER_TENSOR → 改用 TFLite Micro INT8
- INT8 模型可用 `tools/convert_emotion_int8.py` 从 ONNX 重新导出

### SD 卡模型存放
所有模型放在 `/sdcard/models/` 目录下:
- `human_face_detect_msr_s8_v1.espdl`
- `mnp_s8_v1.espdl`
- `emotion_cnn_aug_int8.tflite`

### WiFi / LLM 集成

WiFi + LLM 模块代码已接入 `app_main.c`，UI 已就绪，但 **WiFi 扫描/连接功能不可用**。

#### 硬件约束

ESP32-P4 芯片无原生 WiFi。本板通过 **ESP32-C6 协处理器** 提供 WiFi，C6 与 P4 之间使用 **SDIO** 通信（SDMMC Slot 1）。SD 卡使用 **SDMMC Slot 0**。两个 Slot 共享同一控制器。

#### 当前状态

WiFi 使用标准 `esp_wifi` API（`wifi.c`），该 API 在 P4 上无法直接驱动 C6。正确路径是使用 `espressif/esp_wifi_remote`（v0.13.*）+ `espressif/esp_hosted`（v2.11.*），已添加到 `main/idf_component.yml`。

已完成的接入步骤：
1. `idf_component.yml` 添加 `esp_wifi_remote` + `esp_hosted` 依赖
2. `wifi.c` 在 `esp_wifi_init()` 前调用 `esp_wifi_remote_init(&cfg)`
3. `sdkconfig` 添加 `CONFIG_ESP_WIFI_REMOTE_ENABLED=y` 等选项

#### 当前症状

启动日志中 `esp_wifi_remote_init` 仍然走弱函数桩（`esp_wifi_remote_weak: esp_wifi_remote_init unsupported`），`esp_hosted` 提供的真实 SDIO 实现未被链接。尝试 `esp_hosted_init()` 但因头文件路径不可达未成功。

#### 下一步

- 验证 C6 是否已烧录 `esp_hosted` slave 固件（出厂预烧或需手动烧录）
- 确认 `esp_hosted` 组件在 Kconfig 正确配置后能否自动替换弱函数桩
- 可参考 `host_sdcard_with_hosted` 官方例程了解 SDMMC 双 Slot 共存配置

#### 凭据配置

凭据以宏定义形式写在 `app_main.c` 顶部：
```c
#define WIFI_SSID       "your_wifi_ssid"
#define WIFI_PASSWORD   "your_wifi_password"
#define LLM_API_KEY     "sk-your-api-key"
#define LLM_BASE_URL    LLM_BASE_DEEPSEEK
#define LLM_MODEL_NAME  LLM_MODEL_DEEPSEEK_CHAT
```

建议任务：`llm_suggestion_task` 每 30 秒调用大模型，生成情绪感知陪伴提示并更新 UI 建议栏。

## 已知问题

### 已修复
- ~~TFLite Micro INT8 延迟 ~130ms~~ → 切换为 ESP-DL PER_TENSOR 模型 `emotion_cnn_aug_pt.espdl`，推理 <10ms
- ~~SD 卡 LFN 禁用导致模型加载失败~~ → `CONFIG_FATFS_LFN_HEAP=y`
- ~~`faces` 局部变量未初始化导致随机崩溃~~ → `= {0}` (2026-07-24)

### 待解决
- **WiFi 扫描不可用**: 见上方 "WiFi / LLM 集成"
- 多人脸场景只处理第一张脸
- `espressif/openai` 组件默认超时 60 秒 (在 `OpenAI_Request` 中硬编码)
- Async LLM 请求仅支持单并发 (`s_ctx.busy` 标志)
- 对话历史存储在 PSRAM，受 `MAX_HISTORY_SLOTS` (40条=20轮) 限制
- ISP 色彩未经校准，相机画面偏绿（不影响功能）

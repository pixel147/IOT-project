# 小智（XiaoZhi）语音模型接入主项目 — 技术总结

## 一、项目背景

- **主控芯片**：ESP32-P4（rev v1.3，16MB Flash + 32MB PSRAM）
- **开发框架**：ESP-IDF v5.5.4
- **已实现功能**：LVGL 图形界面（AI 聊天/家居控制/情绪监控）、
  摄像头人脸检测与情绪识别、DeepSeek LLM 对话（键盘输入）、Wi-Fi 联网
- **小智参考项目**：https://github.com/78/xiaozhi-esp32

## 二、接入目标

在主项目基础上复用小智的语音能力，实现：
1. 麦克风拾音 → ESP-SR 唤醒词检测
2. 唤醒后 WebSocket/MQTT 连接小智云端服务器
3. Opus 编码音频流上传
4. 服务器返回语音识别文本（STT），显示在聊天界面
5. 保持原有所有功能不变

> 硬件未接扬声器，语音回复以文字输出到聊天框。

## 三、修改历史

| 版本 | 提交 | 说明 |
|------|------|------|
| v0 | `e9ee466` | WIFI+LLM 成功版（接入前基础） |
| v1 | `e3bc3d9` | 小智模型接入 — 初始 WebSocket 实现 |
| v2 | `ada82f3` | 小智对话残缺版 — 修正记录 |
| v3 | `34cf7b8` | OTA注册+MCP握手+MAC修复 |
| v4 | `8663762` | MQTT+UDP协议实现 |
| v5 | `67bc3c8` | MQTT时序修复+hello_sent标志 |
| v6 | `c8f2645` | 处理服务器推送消息时序 |

## 四、新增/修改文件

| 文件 | 作用 |
|------|------|
| `main/voice/voice_xiaozhi.cpp` | 语音核心：硬件初始化、AFE/WakeNet、Opus、MQTT+UDP |
| `main/voice/voice_xiaozhi.h` | 对外接口声明 |
| `main/CMakeLists.txt` | 添加 esp_http_client、mqtt 依赖；移除旧 WebSocket 组件 |
| `main/app_main.c` | 启动 voice_xiaozhi_start()，添加音乐按钮回调 |
| `partitions.csv` | 添加 model 分区 (1MB) 存放 WakeNet 模型 |
| `components/espressif__openai/` | 修改后的 openai 组件（HTTP 超时 15s） |
| `docs/xiaozhi_integration_summary.md` | 本文件 |

## 五、技术架构

### 5.1 当前架构（2026-07-26）

```
app_main()
  ├─ 显示/相机/人脸检测/情绪识别（原有，不变）
  ├─ WiFi → LLM 初始化
  └─ voice_xiaozhi_start()
       └─ voice_hardware_init_task() [24KB 栈]
            ├─ NVS 读取凭据
            ├─ I2C / I2S / ES8311 Codec
            ├─ ESP-SR AFE (WakeNet "你好小智" + VAD)
            └─ Opus 编码器
            └─ voice_task() [32KB 栈] ← (外层面连接循环)
                 ├─ OTA 注册 (首次启动)
                 │    └─ HTTP POST → api.tenclass.net/xiaozhi/ota/
                 │         → 返回 MQTT 凭据 / WebSocket 凭据
                 ├─ MQTT 连接 → mqtt.xiaozhi.me:8883 (TLS)
                 │    └─ broker 按 client_id 自动路由消息
                 ├─ 发送 hello (transport:"udp")
                 ├─ 接收 server hello → UDP 配置
                 ├─ UDP 音频通道 (AES-128-CTR 加密)
                 └─ 收到 goodbye → 清理 → 重连
```

### 5.2 两套协议实现

| 协议 | 状态 | 说明 |
|------|------|------|
| **WebSocket** | 保留（降级用） | 连 `wss://...`，token=`test-token`。服务器限制功能 |
| **MQTT+UDP** | 主要实现 | 连 `mqtt.xiaozhi.me:8883`，设备专属凭据。UDP 音频 AES 加密 |

### 5.3 消息流程 (MQTT+UDP)

```
 voice_task                     MQTT Broker                  XiaoZhi Server
    |                              |                              |
    |--- CONNECT (client_id) ---->|                              |
    |<-- CONNACK -----------------|                              |
    |                              |                              |
    |--- PUBLISH "device-server" -|---- hello JSON ------------>|
    |   {"transport":"udp",...}   |                              |
    |                              |                              |
    |<-- 按 client_id 自动路由 ----|---- server hello JSON ------|
    |   {"session_id","udp":{     |                              |
    |    "server","port",         |                              |
    |    "key","nonce"}}          |                              |
    |                              |                              |
    |--- MCP initialize respond ->|                              |
    |                              |                              |
    | 创建 UDP socket              |                              |
    | 连接 UDP server:port        |                              |
    |                              |                              |
    | 唤醒词检测 → listen start  |                              |
    |--- UDP Opus+AES 音频 ------>|                              |
    |<-- UDP Opus+AES 音频 ------| (TTS 播放)                   |
    |                              |                              |
    |<-- JSON: STT/TTS msg ------|                              |
```

## 六、关键技术实现

### 6.1 OTA 注册 (xiaozhi_ota_register)

```cpp
// HTTP POST to https://api.tenclass.net/xiaozhi/ota/
// 发送设备信息（MAC、UUID、芯片型号、固件版本等）
// 服务器返回：
// {
//   "mqtt": {"endpoint","client_id","username","password","publish_topic"},
//   "websocket": {"url","token"},  // test-token (仅调试)
//   "activation": {"code",...}     // 首次注册时需要激活
// }
// 结果存入 NVS "mqtt" namespace
```

- **C6 WiFi MAC 修正**：用 `esp_wifi_get_mac()` 取代 `esp_read_mac(ESP_MAC_WIFI_STA)`
- **后台任务执行**：HTTPS 需要大栈，放在 voice_task 中执行（32KB 栈）

### 6.2 MQTT 协议

- **连接**：`mqtts://mqtt.xiaozhi.me:8883`（TLS，用 `esp_crt_bundle_attach` 验证证书）
- **认证**：OTA 返回的 username + password（Base64 编码的 JWT 格式）
- **发布主题**：`"device-server"`（所有 JSON 信号都发到此 topic）
- **接收**：**无需 subscribe** — MQTT broker 按 client_id 自动路由消息给设备
- **心跳保活**：默认 240s（由 OTA 配置）

### 6.3 UDP 音频通道

由 server hello `udp` 字段配置：

```json
{
  "udp": {
    "server": "<IP地址>",
    "port": <端口>,
    "key": "<16字节AES密钥Hex>",
    "nonce": "<16字节nonce Hex>"
  }
}
```

- **加密**：AES-128-CTR（使用 mbedtls）
- **nonce 格式**：`[2B固定][2B长度][4B固定][4B时间戳][4B序号]`
- **发送**：`[16B nonce][加密Opus负载]`
- **接收**：`[1B type][1B flags][2B len][4B ssrc][4B ts][4B seq][加密负载]`

### 6.4 MCP 握手

服务器在 hello 后立即发送 MCP initialize，设备需回复：

```
服务器 → MCP initialize (id:1)
设备   → MCP initialize response (protocolVersion, capabilities)
服务器 → MCP notifications/initialized
服务器 → MCP tools/list (id:2)
设备   → MCP tools/list response (空数组)
```

### 6.5 唤醒词处理

- **唤醒词**：`"你好小智"`（WakeNet9 模型）
- **唤醒后跳过唤醒词语音**：`post_wake_skip = 5`（丢弃~300ms 音频，避免唤醒词混入语音流）
- **语音输入**：唤醒后直接说话，无需按按钮

### 6.6 断线重连

```
voice_task 外层 while(1):
  ├─ OTA 注册（仅首次）
  ├─ 尝试连接（MQTT）
  │    ├─ 成功 → 进入内层 while（处理音频）
  │    └─ 失败 → 等待 3 秒（可被按钮中断）→ 重试
  └─ 断线 → 清理 → 回到外层头部 → 重连
```

## 七、依赖

| 组件 | 版本 | 用途 |
|------|------|------|
| `espressif/esp-sr` | ~2.4.7 | ESP-SR AFE + WakeNet |
| `espressif/esp_audio_codec` | ~2.5.0 | Opus 编码器 |
| `esp_http_client` | 内置 | OTA 注册 (HTTPS) |
| `mqtt` | 内置 | MQTT 客户端 (TLS) |
| `mbedtls/aes.h` | 内置 | UDP 音频 AES-128-CTR |
| `espressif/esp_websocket_client` | ~1.5.0 | WebSocket 降级方案 |

## 八、已知问题

| 问题 | 状态 | 说明 |
|------|------|------|
| **C6 WiFi 不稳定** | ❌ | 启动时断连 (reason=2/205)，DNS 间歇性失败。根在 SDIO 链路 |
| **MQTT 服务器断连** | ❌ | 连上后服务器 ~3s 后发 EOF。可能是凭据问题或协议版本不匹配 |
| **WebSocket test-token** | ❌ | 服务器返回的 WebSocket token 为 test-token，所有设备共用 |
| **MQTT 凭据来源** | ❌ | OTA 注册返回的凭据在 WiFi 不稳定时无法获取 |
| **语音合成播放** | ❌ | 无扬声器，TTS 仅文字输出 |
| **多人脸处理** | ❌ | 只处理第一张人脸 |
| **唤醒词"小智小智"** | ⚠️ | 文档中写的是"小智小智"，实际模型是**"你好小智"** |
| **字体 510 字限制** | ⚠️ | LVGL cmap 9-bit 限制 |

## 九、启动流程（详细）

```
硬件上电
  └─ bootloader → 分区表加载
       └─ factory app (6MB)
            ├─ 显示初始化 (DSI LCD EK79007)
            ├─ 三缓冲分配 (PSRAM, 3×614KB)
            ├─ LVGL 屏幕创建（Home→Monitor→Chat→Settings）
            ├─ SD 卡挂载 → 模型加载
            │    ├─ 人脸检测模型 (ESP-DL)
            │    └─ 情绪识别模型 (ESP-DL INT8)
            ├─ WiFi 初始化 (C6 via SDIO)
            │    ├─ wifi_init() → STA idle
            │    └─ wifi_connect() → NVS 凭据自动连接
            ├─ 摄像头启动 (MIPI-CSI, 640×480 @ 30fps)
            ├─ LLM 初始化 (DeepSeek API)
            ├─ XiaoZhi 语音启动
            │    ├─ voice_hardware_init_task [24KB]
            │    │    ├─ I2C/I2S/ES8311 初始化
            │    │    ├─ WakeNet+AFE 初始化
            │    │    └─ Opus 编码器初始化
            │    └─ voice_task [32KB] (重连循环)
            │         ├─ OTA 注册 (HTTPS, 仅首次)
            │         ├─ MQTT 连接 (mqtts://mqtt.xiaozhi.me:8883)
            │         │    ├─ 发送 hello → 等待 server hello
            │         │    ├─ MCP 初始化响应
            │         │    ├─ UDP 音频通道建立
            │         │    └─ 等待唤醒词 / 音频处理
            │         └─ 断线 → 清理 → 重连
            ├─ 情绪识别任务 (每2秒, Core 0)
            └─ 相机回调 (Core 1, 每帧)
```

## 十、关键代码片段

### VoiceState 结构体

```cpp
struct VoiceState {
    /* 硬件 */
    esp_codec_dev_handle_t codec;
    i2s_chan_handle_t tx, rx;
    srmodel_list_t *models;
    const esp_afe_sr_iface_t *afe_iface;
    esp_afe_sr_data_t *afe;
    void *opus;
    
    /* 状态 */
    EventGroupHandle_t events;
    char session_id[96];
    char device_id[18];
    std::atomic<bool> manual_start_requested, ota_done, hello_sent;
    bool listening;
    int silence_frames, post_wake_skip;
    std::vector<int16_t> opus_pcm;
    
    /* MQTT */
    esp_mqtt_client_handle_t mqtt;
    char mqtt_endpoint[128], mqtt_client_id[128];
    char mqtt_username[128], mqtt_password[128];
    char mqtt_publish_topic[64];
    
    /* UDP 音频 */
    int udp_fd;
    char udp_server[64];
    int udp_port;
    uint8_t aes_key[16], aes_nonce[16];
    mbedtls_aes_context aes_ctx;
    uint32_t local_sequence;
};
```

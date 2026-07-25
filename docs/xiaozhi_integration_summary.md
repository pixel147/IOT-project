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
2. 唤醒后 WebSocket 连接小智云端服务器
3. Opus 编码音频流上传
4. 服务器返回语音识别文本（STT），显示在聊天界面
5. 保持原有所有功能不变

> 硬件未接扬声器，语音回复以文字输出到聊天框。

## 三、新增/修改文件

| 文件 | 作用 |
|------|------|
| main/voice/voice_xiaozhi.cpp | 语音核心：I2S 采集、AFE/WakeNet、WebSocket、Opus |
| main/voice/voice_xiaozhi.h | 对外接口声明 |
| partitions.csv | 添加 model 分区 (1MB) 存放 WakeNet 模型 |
| main/app_main.c | 调用 voice_xiaozhi_start()，添加音乐按钮回调 |
| CMakeLists.txt | 添加 ESP-SR、opus、websocket 依赖 |

## 四、技术架构

### 4.1 启动流程

```
app_main()
  → voice_xiaozhi_start()
    → voice_hardware_init_task()
      ├─ NVS 读取凭证 (url/token/uuid)
      ├─ I2C 初始化
      ├─ I2S 配置 (16kHz, 16bit, MONO, DMA)
      ├─ ES8311 Codec 初始化
      ├─ ESP-SR AFE (WakeNet + VAD)
      └─ Opus 编码器
    → voice_task() 主循环
```

### 4.2 语音主循环

```
voice_task()
  ├─ WebSocket 连接小智服务器
  ├─ 发送 hello 握手
  └─ while(已连接)
       ├─ i2s_channel_read() → PCM 音频
       ├─ afe->feed() → AFE 处理
       ├─ afe->fetch()
       ├─ 检测唤醒词 → 开始录音
       ├─ Opus 编码 → WebSocket 发送
       └─ VAD 静音检测 → 停止录音
```

## 五、关键踩坑与解决方案

### 5.1 编译错误

| 问题 | 修复 |
|------|------|
| afe_fetch_result_t 无 wakenet_state | 改为 wakeup_state |
| JSON 引号未转义 | 使用 \" 正确转义 |

### 5.2 运行时问题

| 问题 | 根因 | 修复 |
|------|------|------|
| codec_read 返回 0 | esp_codec_dev_read 在 P4+ES8311 上不兼容 | 改用 i2s_channel_read 直读 |
| AFE ringbuffer 空 | feed() 未收到数据 | 同上 |
| 声道不匹配 | I2S STEREO 但 codec 是 MONO | 改为 I2S_SLOT_MODE_MONO |
| 堆分配性能问题 | 每次循环 malloc/free | 预分配静态缓冲区 |
| init 后崩溃 | 任务函数返回 | while(1) 保持任务存活 |

## 六、硬件配置

- ES8311 Codec: 16kHz, 16bit, MONO, 增益 30dB
- I2S: MONO 模式, DMA 240帧×6描述符
- WakeNet 模型: wn9_nihaoxiaozhi_tts (唤醒词: 小智小智)
- 服务器: wss://api.tenclass.net/xiaozhi/v1/

## 七、后续优化

1. 添加 I2S DAC 输出实现扬声器 TTS 播放
2. 训练自定义唤醒词
3. 启用 AEC/NS 提升通话质量
4. 低功耗 ULP 唤醒
5. OTA 固件升级

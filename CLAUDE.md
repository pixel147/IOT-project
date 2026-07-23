# CLAUDE.md — 情绪守护项目

## 项目概述

基于 ESP32-P4-Function-EV-Board 的情绪识别 + 大模型聊天终端。
CSI 摄像头实时捕获 → 人脸检测 → 情绪分类 → 大模型聊天。

**目标**: 情绪监管与陪伴聊天
**框架**: ESP-IDF v5.5.4, LVGL v9.5.0 (`esp_lvgl_port` v2.8.0)
**硬件**: ESP32-P4-Function-EV-Board, DSI LCD (EK79007), MIPI-CSI 摄像头 (SC2336/OV5647)

---

## 当前架构 (2026-07-22)

```
Camera → CSI V4L2 → 30fps RGB565
  │
  ├─ Camera Callback (Core 1, 30fps)
  │    ├─ memcpy → s_fb_disp (显示缓冲)
  │    ├─ face_detect_run() [ESP-DL, ~32ms]
  │    ├─ draw_rect_rgb565() + ui_update_emotion()
  │    └─ lv_refr_now()
  │
  └─ emotion_task (Core 0, 每2秒)
       ├─ memcpy face ROI → roi_buf
       ├─ emotion_tflite_run() [TFLite Micro INT8, ~130ms]
       └─ update s_ai_classes[] (供相机回调读取)
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

## 关键文件

| 文件 | 说明 |
|------|------|
| `main/app_main.c` | 入口: 显示初始化, 三缓冲, 相机回调, 人脸检测, 情绪任务, 监控按钮控制 |
| `main/ui/ui.c` | LVGL UI: 暗色主题, 三栏布局, 情绪徽章(左上角), 图例(左竖排), 信息面板(右), 建议栏 |
| `main/ui/ui.h` | UI 公共 API (含 `ui_update_emotion()`, `ui_hide_emotion()`, `ui_is_monitoring()`) |
| `main/ui_font_zh_22.c` | 22px 中文字体 (220 字, 由 `tools/generate_ui_font.py` 从 Deng.ttf 生成) |
| `main/camera/camera.c` | CSI V4L2 摄像头驱动 (MIPI-CSI RGB565, MMAP 三缓冲) |
| `main/sdcard/sdcard_init.cpp` | SDMMC 挂载到 `/sdcard` |
| `main/model_loader/face_detect_wrapper.cpp` | ESP-DL 人脸检测 (HumanFaceDetect MSRMNP_S8_V1) |
| `main/model_loader/emotion_tflite.cpp` | TFLite Micro 情绪识别 INT8 (48×48 灰度, 7 类, ~130ms) |
| `main/llm/llm_client.c` | (预留) 大模型 HTTPS 客户端, 百度 AI Studio |
| `main/llm/llm_config.h` | (预留) WiFi 密码 + API Key 配置 |
| `main/wifi/wifi.c` | (预留) WiFi Station 连接 (基于官方 station 例程) |
| `tools/generate_ui_font.py` | 字体生成脚本, 编辑 TEXT 字串后运行 |
| `tools/convert_emotion_int8.py` | ONNX → INT8 TFLite 模型转换脚本 |
| `ESP-DL_COMPAT.md` | ESP-DL 模型(per-channel)不兼容性说明 |

## 模型

| 功能 | 框架 | 模型路径 (SD卡) | 延迟 |
|------|------|----------------|:----:|
| 人脸检测 | ESP-DL | `/sdcard/models/human_face_detect_msr_s8_v1.espdl` + `mnp_s8_v1.espdl` | ~32ms |
| 情绪识别 | TFLite Micro INT8 | `/sdcard/models/emotion_cnn_aug_int8.tflite` (95KB) | ~130ms |

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
| `esp_http_client` + `json` | (预留) LLM HTTPS |
| `human_face_detect` | 人脸检测 ESP-DL |

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
`main/llm/` 和 `main/wifi/` 模块已创建但未接入 app_main.c。
使用时需:
1. 修改 `llm_config.h` 填入 WiFi SSID/密码 + API Key
2. 在 `app_main.c` 中调用 `wifi_init_sta()` + `llm_init()`
3. CMakeLists.txt 已包含源文件和依赖

## 已知问题

- TFLite Micro INT8 延迟 ~130ms (可用但非最优)
- 多人脸场景只处理第一张脸
- WiFi/LLM 需用户自行填入凭据
- 建议栏文字使用 `LV_LABEL_LONG_SCROLL_CIRCULAR` 自动滚动

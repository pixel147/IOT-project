# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Smart Vision Pose Assessment → Emotion + LLM Chat Terminal** (in transition).

Originally a real-time exercise pose evaluator, now pivoting toward an emotion-detection-enabled LLM chat terminal on ESP32-P4. The pose pipeline was too slow; the new direction uses a lightweight EmotionCNN for face-based emotion recognition paired with a WiFi-connected LLM chat UI.

**Target**: ESP32-P4-Function-EV-Board with DSI LCD (EK79007), MIPI-CSI camera (OV5647/SC2336)  
**Framework**: ESP-IDF v5.5.4, LVGL v9.5.0 (`esp_lvgl_port` v2.8.0)

## Project Status (2025-07-22)

| Component | Status | Notes |
|-----------|--------|-------|
| MIPI-CSI camera | ✅ Working | V4L2 MMAP, RGB565, ~30fps |
| LVGL UI | ✅ Working | Pose skeleton drawing, camera preview, angles panel |
| MoveNet pose inference | ✅ Working | TFLite Micro, ~100-200ms/inference |
| EmotionCNN inference | ⏸️ Files exist, not wired | `emotion_estimator.*` in model_loader/ but not in CMakeLists |
| LLM chat terminal | ❌ Not started | — |

## Build Commands

```powershell
# Activate ESP-IDF (required once per PowerShell session)
. "G:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1"

# Build for target board
idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.esp32_p4_function_ev_board build

# Build + flash + monitor
idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.esp32_p4_function_ev_board -p PORT flash monitor
```

The active board config is `sdkconfig.bsp.esp32_p4_function_ev_board`. `sdkconfig.defaults` provides the base.

## Key Source Files

| File | Status | Purpose |
|------|--------|---------|
| `main/app_main.c` | Active | Entry — display, dual PSRAM buffer, camera, pose inference task |
| `main/CMakeLists.txt` | Active | Build config — MoveNet TFLite path (emotion_estimator NOT included) |
| `main/camera/camera.c` | Active | MIPI-CSI V4L2 driver — MMAP capture, PSRAM fb, Core 1 capture task |
| `main/camera/camera.h` | Active | Camera API |
| `main/model_loader/pose_estimator.cpp` | Active | MoveNet TFLite Micro inference (was YOLO ESP-DL) |
| `main/model_loader/pose_estimator.hpp` | Active | C/C++ bridge for pose estimator |
| `main/model_loader/emotion_estimator.cpp` | **Unused** | EmotionCNN ESP-DL inference — not in CMakeLists |
| `main/model_loader/emotion_estimator.hpp` | **Unused** | C/C++ bridge for emotion estimator |
| `main/sdcard/sdcard_init.cpp` | Active | SDMMC mount via BSP (`/sdcard`) |
| `main/ui/ui.c` | Active | LVGL UI — skeleton, camera preview, angles panel |
| `main/ui/ui.h` | Active | UI API |
| `main/ui/ui_font_zh_22.c` | Active | Chinese font (~100 CJK chars from Deng.ttf) |
| `partitions.csv` | Active | 6M factory + 2M SPIFFS |
| `sdkconfig.defaults` | Active | ESP32-P4, MIPI-CSI, PSRAM, LVGL9 config |
| `tools/generate_ui_font.py` | Active | Font generator from `C:\Windows\Fonts\Deng.ttf` |

## Architecture (Current)

### Startup flow
```
bsp_display_start_with_config() → DSI LCD + LVGL
  → heap_caps_aligned_alloc × 2 (PSRAM dual buffer: 640×480×2 each)
  → ui_init() + fps_timer_cb
  → sdcard_init() → SD card mount at /sdcard
  → pose_estimator_load_and_print() → MoveNet TFLite model load
  → cam_start(640, 480, 30, on_camera_frame) → MIPI-CSI V4L2
  → xTaskCreatePinnedToCore(pose_inference_task, Core 1, prio 5)
```

### Camera pipeline (MIPI-CSI V4L2)
```
bsp_camera_start() → open("/dev/video0") → VIDIOC_S_FMT (RGB565)
  → alloc_psram_fb() → VIDIOC_REQBUFS(3) → mmap × 3 → VIDIOC_STREAMON
  → xTaskCreatePinnedToCore(cam_capture_task, Core 1)
     → loop: DQBUF → memcpy to PSRAM fb → callback → QBUF
```

### Pose inference task (Core 1, ~10fps cap)
```
loop:
  wait for s_ready_idx >= 0
  pose_estimator_run(s_fb[ridx], w, h, stride, joints, confs, &score)
  if OK: ui_update_skeleton() + deviations
  else:  ui_update_skeleton(NULL) (clear)
  vTaskDelay(100ms)
```

### Current memory layout (MIPI-CSI + MoveNet)

| What | Where | Size |
|------|-------|------|
| Model (MoveNet TFLite) | SD card | ~2.9 MB |
| `s_fb[0]` | PSRAM, 64B-aligned | 640×480×2 = 614 KB |
| `s_fb[1]` | PSRAM, 64B-aligned | 640×480×2 = 614 KB |
| `s_cam.fb_psram` | PSRAM, 32B-aligned | 640×480×2 = 614 KB |
| Tensor arena (TFLite) | PSRAM | 2 MB |
| SPIFFS | Flash | 2 MB |
| Factory app | Flash | 6 MB |

### Comparison with previous YOLO/UVC architecture

| Aspect | Old (YOLO + UVC) | Current (MoveNet + MIPI-CSI) |
|--------|-------------------|------------------------------|
| Camera | USB UVC (LRCP U3-JX02) | MIPI-CSI (OV5647/SC2336) |
| Frame format | MJPG → HW JPEG decode → RGB565 | RGB565 direct from ISP |
| Inference engine | ESP-DL (`dl::Model`) | TFLite Micro (`tflite::MicroInterpreter`) |
| Model | YOLO26n-pose (~3.5MB .espdl) | MoveNet Lightning (~2.9MB .tflite) |
| Input size | 640×640 letterbox | 192×192 |
| Build deps | `esp-dl`, `usb_host_uvc`, `esp_new_jpeg` | `esp-tflite-micro` |
| Demo mode | Yes (animated skeleton) | No |

## Previous YOLO/UVC Debugging (archived, for reference)

The original USB UVC implementation was abandoned due to ISOC transfer corruption on ESP32-P4's DWC2 USB host. Key findings:
- Camera: LRCP U3-JX02, MJPG 640×480 @ 30fps, ~50KB frames
- Root cause: DWC2 ISOC scheduling cannot reliably handle UVC streams
- MIPI-CSI migration eliminated all USB/UVC/JPEG issues

## SD Card Models (E:/models/)

| File | Size | Status |
|------|------|--------|
| `movenet_singlepose_lightning_int8.tflite` | 2.9 MB | ✅ Active |
| `emotion_cnn_aug.espdl` | 106 KB | ⏸️ Ready, not integrated |
| `emotion_cnn_80k.espdl` | 106 KB | ⏸️ Alternate weights |
| `yolo26n-pose_esp32p4.espdl` | 3.5 MB | ❌ Deprecated |
| `posenet_mobilenet*.espdl` | 3.6 MB | ❌ Deprecated |
| `coco_pose_yolo11n_pose_*.espdl` | ~3 MB | ❌ Deprecated |

## EmotionCNN Model (ready for integration)

- **Weights**: `emotion_cnn_aug.espdl` (106 KB, INT8)
- **Architecture**: 4 residual blocks, 76,855 params, 1×48×48 grayscale input
- **Output**: 7-class (Angry/Disgust/Fear/Happy/Sad/Surprise/Neutral)
- **Training**: PC-side with strong augmentation (rotation ±12°, scale 0.85-1.15, RandomErasing), 61.4% test accuracy on FER2013
- **ESP-DL API**: `dl::Model` with `fbs::MODEL_LOCATION_IN_SDCARD`
- **Integration files**: `emotion_estimator.cpp/hpp` exist in `main/model_loader/` but need to be wired into CMakeLists.txt and app_main.c

## Planned: Emotion + LLM Chat Terminal

Next steps for the pivot:
1. Wire `emotion_estimator` into the build and inference loop
2. Redesign UI: remove pose skeleton/angles, add chat message list + text input
3. Add WiFi + HTTP client for LLM API calls
4. Add face detection (can reuse nose/eyes from MoveNet, or add lightweight face detector)

## Critical Config

- `CONFIG_SPIRAM=y` + `CONFIG_SPIRAM_MODE_HEX=y` — PSRAM mandatory
- `CONFIG_CAMERA_OV5647=y` + `CONFIG_CAMERA_SC2336=y` — MIPI-CSI auto-detect
- `CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER=y` — ISP RGB565 output
- `CONFIG_LV_CONF_SKIP=y` — custom `lv_conf.h` in `main/`
- `CONFIG_LV_USE_FLOAT=y` — for `lv_point_precise_t`
- `CONFIG_LV_USE_PERF_MONITOR=y` — FPS overlay
- `CONFIG_PARTITION_TABLE_CUSTOM=y` — 6M + 2M SPIFFS

## Key Dependencies (current)

| Component | Purpose |
|-----------|---------|
| `esp32_p4_function_ev_board` | BSP (display, camera, SD) |
| `esp-tflite-micro` | MoveNet inference |
| `esp_lvgl_port` + `lvgl/lvgl` | LVGL graphics |
| `esp_video` | ISP pipeline controller |
| `esp_lcd_ek79007` | DSI LCD driver |

### Removed dependencies (from YOLO/UVC era)

`esp-dl`, `usb_host_uvc`, `esp_new_jpeg`, `esp_driver_jpeg` are no longer required.

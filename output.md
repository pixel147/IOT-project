# UVC 相机预览处理流 — 全链路详解

## 架构总览

```
 USB 摄像头 (LRCP U3-JX02)
        │ USB ISOC (MJPG, 50-60KB/帧)
        ▼
┌─────────────────────────────────────┐
│  UVC 驱动层 (camera.c)              │
│  ├─ uvc_frame_callback()            │ ← ISOC 回调，拷贝到乒乓缓冲
│  ├─ uvc_frame_processing_task()     │ ← JPEG 解码 + 降采样 + 回调
│  └─ cam_start() / cam_stop()        │
└─────────────────────────────────────┘
        │ cam_frame_cb_t → RGB565 640×480
        ▼
┌─────────────────────────────────────┐
│  应用层 (app_main.c)                │
│  ├─ on_camera_frame()               │ ← memcpy 到双缓冲 + lvgl_port_lock
│  │                                   │   + lv_canvas_set_buffer()
│  └─ fps_timer_cb()                  │ ← 1s 定时更新 FPS 显示
└─────────────────────────────────────┘
        │ lv_canvas → draw buffer
        ▼
┌─────────────────────────────────────┐
│  LVGL 渲染层 (esp_lvgl_port)        │
│  ├─ 200 行 PSRAM 双缓冲             │
│  ├─ LV_DISPLAY_RENDER_MODE_PARTIAL  │ ← 仅渲染脏区
│  └─ EK79007 DSI LCD flush           │
└─────────────────────────────────────┘
```

---

## 1. 相机驱动层 (`camera.c`)

### 1.1 初始化流程

```
cam_start(640, 480, 30, on_camera_frame)
  ├─ usb_host_install()                    ← USB Host 库
  ├─ uvc_host_install()                     ← UVC 驱动
  ├─ 等待相机连接（5s 超时）
  ├─ select_mjpeg_format()                  ← 优先 MJPG ≤30fps
  │   └─ 回退: select_yuy2_format()
  ├─ allocate_rgb565_buffer(614KB)          ← RGB565 输出缓冲
  ├─ mjpg_frame_buf[2] × 200KB              ← MJPG 乒乓输入缓冲
  ├─ uvc_host_stream_open()                 ← 4 URB × 16KB
  └─ uvc_frame_processing_task()            ← 创建处理任务
```

### 1.2 ISOC 帧接收

```
uvc_frame_callback(frame)
  ├─ [MJPG] memcpy → mjpg_frame_buf[write_idx]
  │         set frame_ready = true
  │         return true (立即归还 UVC buffer)
  └─ [YUY2] xQueueSend → frame_queue
            return false (暂不归还)
```

### 1.3 帧处理任务（核心）

```
uvc_frame_processing_task()
  while (true):
    [MJPG 路径]
    if (!frame_ready): vTaskDelay(1ms); continue
    frame_ready = false
    r = mjpg_write_idx                     ← 取当前写完的缓冲
    mjpg_write_idx ^= 1                    ← 交换，回调写另一块
    
    ① esp_cache_msync(src, DIR_C2M | TYPE_DATA)   ← 写回输入
    ② jpeg_decoder_get_info()                      ← 解析 JPEG 头
    ③ 按实际分辨率分配 jpeg_out_buf
    ④ esp_cache_msync(out, DIR_M2C | INVALIDATE)   ← 失效输出缓存
    ⑤ jpeg_decoder_process()                        ← HW JPEG 解码 (1280×1024)
    ⑥ esp_cache_msync(out, DIR_M2C | INVALIDATE)   ← 刷新 CPU 缓存
    ⑦ ½ 降采样 1280×1024 → 640×480
    ⑧ esp_cache_msync(rgb565, DIR_C2M | TYPE_DATA) ← 写回降采样结果
    ⑨ s_cam.callback(rgb565_buffer, ...)            ← → on_camera_frame()
    
    [YUY2 路径] (当前未使用)
    xQueueReceive → yuy2_to_rgb565() → callback()
```

### 1.4 缓存同步（三步法，参考 ESP Techpedia）

| 步骤 | 时机 | 操作 | 目的 |
|------|------|------|------|
| ① | 解码前 | `DIR_C2M + TYPE_DATA` | 确保输入数据写入 PSRAM |
| ② | 输出缓冲就绪 | `DIR_M2C + INVALIDATE` | 失效旧缓存，让 DMA 直写 |
| ③ | 解码后 | `DIR_M2C + INVALIDATE` | 刷新 CPU 缓存读 DMA 结果 |
| ④ | 降采样后 | `DIR_C2M + TYPE_DATA` | 写回调数据供 LVGL 读取 |

### 1.5 关键配置

| 参数 | 值 | 说明 |
|------|-----|------|
| URB 数量 | 4 | USB ISOC 传输缓冲数 |
| URB 大小 | 16 KB | 单个 URB 大小 |
| MJPG 乒乓缓冲 | 2 × 200 KB | 防止解码/写入竞态 |
| JPEG 解码器 | ESP-IDF `driver/jpeg_decode` | 替代 `esp_new_jpeg` |
| JPEG 源分辨率 | 1280×1024 | 摄像头原生 sensor 尺寸 |
| 降采样输出 | 640×480 RGB565 | ½ 最近邻 |
| 处理任务栈 | 8 KB | UVC_TASK_PRIORITY=10 |

---

## 2. 应用层 (`app_main.c`)

### 2.1 相机帧回调

```
on_camera_frame(buf, len, w=640, h=480, stride=1280, fmt=RGB565)
  ├─ memcpy → s_fb[idx] (双缓冲，PSRAM)
  ├─ esp_cache_msync(s_fb[idx], DIR_C2M | TYPE_DATA)
  ├─ s_ready_idx = idx
  └─ if lvgl_port_lock(0) 成功:          ← 非阻塞尝试锁
       esp_cache_msync(s_fb[ridx], DIR_M2C | INVALIDATE)
       ui_update_camera_preview()          ← lv_canvas_set_buffer()
       lvgl_port_unlock()
     else: 锁失败则跳过（下帧再试）
```

### 2.2 UI 更新

```
ui_update_camera_preview(buf, w, h, stride)
  ├─ lv_canvas_set_buffer(camera_img, buf, w, h, RGB565)
  └─ lv_obj_invalidate(camera_img)         ← 标记脏区
```

### 2.3 显示配置

| 参数 | 值 | 说明 |
|------|-----|------|
| draw buffer | 1024×200 像素 ×2 | 200 行 PSRAM 双缓冲 |
| 渲染模式 | LV_DISPLAY_RENDER_MODE_PARTIAL | 仅渲染脏区 |
| LVGL 锁 | lvgl_port_lock(0) | 非阻塞，和 LVGL 任务互斥 |
| 相机分辨率 | 640×480 @ 30fps | 请求值 |

---

## 3. 数据流转（一帧的完整生命周期）

```
时间轴 →

[USB ISOC]
  UVC 相机 → DWC2 控制器 → URB 缓冲 → uvc_frame_callback
  → memcpy mjpg_frame_buf[0] (~50KB)  耗时: ~2ms

[处理任务] (独立任务, Core 0/1, 优先级 10)
  frame_ready→ 读 mjpg_frame_buf[0]
  ① cache writeback
  ② JPEG header parse                  耗时: ~1ms
  ③ JPEG decode (1280×1024 HW)         耗时: ~15ms
  ④ cache invalidate
  ⑤ 降采样 1280→640                    耗时: ~3ms
  ⑥ cache writeback
  → callback(rgb565, 640×480)

[应用回调] (处理任务上下文)
  → memcpy s_fb[0]                     耗时: ~1ms
  → lvgl_port_lock(0)
    成功: lv_canvas_set_buffer() + invalidate → LVGL 渲染
    失败: 跳帧

[LVGL 渲染] (LVGL 任务)
  → 3 pass × 200 行 PSRAM 渲染         耗时: ~20-40ms
  → DSI DMA flush (200 行/pass) → LCD
```

### 数据拷贝次数

1. ISOC URB → mjpg_frame_buf (CPU memcpy, ~50KB)
2. JPEG decode → jpeg_out_buf (DMA, ~2.5MB)
3. jpeg_out_buf → rgb565_buffer (CPU 降采样, ~614KB)
4. rgb565_buffer → s_fb[idx] (CPU memcpy, ~614KB)
5. s_fb[idx] → LVGL draw buffer (LVGL 软件渲染, ~614KB)
6. LVGL draw buffer → LCD (DSI DMA, ~1.2MB)

总计：~3 次 CPU PSRAM 拷贝 + 1 次 DMA + 1 次 LVGL 渲染 + 1 次 DSI flush

---

## 4. UI 控件树

```
Screen (#0A0E27, flex column)
  ├── Status bar (46px)        — 时间, FPS, 模式, 状态
  ├── Middle row (flex-grow)
  │     ├── Left menu (132px)  — 俯卧撑/引体向上/仰卧起坐
  │     ├── Pose stage (flex-grow)
  │     │     ├── camera_container (透明背景)
  │     │     │     └── camera_img = lv_canvas    ← 相机预览
  │     │     └── skeleton_container (透明)
  │     └── Angle panel (174px)
  ├── Suggestion bar
  ├── Control bar (56px)
  └── Message popup (hidden)
```

---

## 5. 文件变更清单

| 文件 | 改动 |
|------|------|
| `main/camera/camera.c` | USB UVC 驱动 + MJPG/JPEG 解码 + 乒乓缓冲 + 降采样 |
| `main/app_main.c` | 双缓冲 + lv_canvas + lvgl_port_lock 同步 |
| `main/ui/ui.c` | lv_image → lv_canvas，新增 ui_get_preview_rect() |
| `main/ui/ui.h` | 新增 ui_get_preview_rect() 声明 |
| `main/CMakeLists.txt` | +usb_host_uvc +esp_driver_jpeg，-esp_new_jpeg -sdcard -model_loader |
| `sdkconfig` | UVC interval 数组=6，draw_buf_align 调整 |
| `sdkconfig.bsp.esp32_p4_function_ev_board` | PPA 尝试记录 |

---

## 6. 已知问题 & 优化方向

| 问题 | 严重度 | 可能方案 |
|------|--------|---------|
| VSYNC 撕裂（200 行 = 3 撕裂带） | 中 | esp_lv_adapter + TE pin |
| 快速运动重影 | 低 | 已用乒乓缓冲缓解 |
| CPU 高（JPEG 解码 + 降采样） | 中 | PPA 硬件加速 |
| UVC ISOC 带宽（ESP32-P4 DWC2 调度深度有限） | 低 | 已验证 MJPG 50KB/帧可行 |

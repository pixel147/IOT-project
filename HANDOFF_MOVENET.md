# MoveNet 手递手文档 — 给下一位 AI 的完整上下文

## 项目概况

**目标**: 在 ESP32-P4 上用 MoveNet SinglePose Lightning 替代 YOLO26n-pose，实现单目人体姿态估计（俯卧撑/引体向上/仰卧起坐评估）。

**状态**: ⚠️ 模型已加载、无 op 报错，但推理时间 ~4.2s（应 ~50-100ms），且输出全部为零（0/17 keypoints）。

---

## 架构变更记录

### 替换前
- `app_main.c` → `pose_estimator_run()` → `dl::Model` (ESP-DL) → YOLO26n-pose.espdl
- CMake 依赖: `espressif/esp-dl ^3.3.6`

### 替换后
- `app_main.c` → `pose_estimator_run()` → `tflite::MicroInterpreter` (esp-tflite-micro) → MoveNet.tflite
- CMake 依赖: `espressif/esp-tflite-micro ^1.3.3`
- 模型: `movenet_singlepose_lightning_int8.tflite` (2.89 MB, int8 量化, 输入 192×192×3 UINT8, 输出 [1,1,17,3] FLOAT32)

### API 兼容
`pose_estimator.hpp` 接口完全不变，`app_main.c` 无逻辑修改。
- `pose_estimator_load_and_print()` → 加载模型到 PSRAM
- `pose_estimator_run(rgb565, w, h, stride, joints[17][2], confs[17], &score)` → 预处理 → 推理 → 输出

---

## 当前症状

### 1. 推理耗时 ~4.2 秒（应 ~50ms）
**从日志看**: `pre=7ms infer=4257ms post=0ms total=4264ms`
预处理 7ms、后处理 0ms 都正常，但 inference 占了 4.2s。

### 2. 输出全部为零
**从日志看**: `Output raw: kpt0=[0.0000,0.0000,0.0000] ... Inference: 0/17 keypoints valid`
`Invoke()` 返回 kTfLiteOk 但输出张量的 score 字段全为 0。

---

## 已尝试的修复及结果

### 阶段一: 模型格式问题
| 尝试 | 结果 |
|------|------|
| float16 模型 (TF Hub) | 152 个 DEQUANTIZE 不支持 float16 → 放弃 |
| int8 模型 (TF Hub, 当前) | 加载成功, 无 op 报错, 但推断 4.2s |

### 阶段二: Op 兼容补丁
在 `managed_components/espressif__esp-tflite-micro/` 中修改：

| 文件 | 修改 | 原因 |
|------|------|------|
| `kernels/cast.cc` | `CastEval()` 和 `copyToTensor()` 增加 `kTfLiteUInt8`、`kTfLiteInt64` | MoveNet 有 5 个 CAST 用 UINT8/INT64 |
| `kernels/floor_div.cc` | `FloorDivEval()` 增加 `kTfLiteInt32` case | MoveNet 的 FLOOR_DIV 用 INT32 |

### 阶段三: 框架问题
| 尝试 | 结果 |
|------|------|
| `set(COMPONENTS main esp-tflite-micro esp-nn)` | 强制编译 esp-nn |
| `add_compile_options(-DESP_NN)` | 全局定义 ESP_NN 宏 |
| `main/CMakeLists.txt REQUIRES esp-nn` | 强制链接 |
| `esp-tflite-micro/CMakeLists.txt PRIV_REQUIRES esp-nn` | 创建链接依赖 |
| 所有修改后 `fullclean` 再编译 | 依然 4.2s |

### 阶段四: 预处理优化
| 尝试 | 结果 |
|------|------|
| 双线性插值 → nearest-neighbor | pre=9ms→7ms 改善不大, 非瓶颈 |

### 阶段五: 自检
`pose_estimator.cpp` 末尾新增 `run_self_test()`：
- 创建 640×480 灰色渐变图
- 调用 `pose_estimator_run()` 3 次
- 打印时间和输出统计
- 尚未看到输出结果（用户编译没通过，正在修复中）

---

## 核心问题分析

### 4.2s 推理 = ESP-NN 加速未生效

EXPECTED time 的计算:
- Model: 50×Conv2D + 24×DepthwiseConv2D + 其他 (int8, 192×192)
- With ESP-NN P4V8 (16 MAC/cycle): ~50-100ms
- Without ESP-NN (reference C code): ~2-5s
- 实测: 4.2s → ✅ 确认 ESP-NN 未生效

**为什么 ESP-NN 可能没生效**:
1. `-DESP_NN` 宏定义虽已加，但 `conv.cc` (esp_nn 版) 的 `#include <esp_nn.h>` 条件编译可能未触发
2. ESP-NN 的 P4 优化内核 (`esp_nn_conv_esp32p4.c`) 是否被编译进最终二进制？
3. 链接顺序：esp-tflite-micro 必须在 esp-nn 之前
4. TensorFlow Lite Micro 的内核注册机制：`Register_CONV_2D()` 符号冲突时可能选了 reference 版

**如何验证**:
- 查看 build 日志中是否编译了 `esp32p4/esp_nn_conv_esp32p4.c` 和 `esp_nn/conv.cc`
- 在 `conv.cc` (esp_nn 版) 中加 `#pragma message` 确认编译
- 用 `riscv32-esp-elf-nm` 检查符号表中是否有 `esp_nn_conv_s8_*` 符号

### 输出全零 = 模型输入无效

即使推理慢，模型也不该输出全零。可能原因：
1. **模型权重未正确加载** — 但 AllocateTensors() 成功
2. **输入帧为全黑** — UVC 丢帧导致 frame buffer 为零
3. **INT8 量化参数错误** — QUANTIZE op 的 scale/zero_point 不匹配
4. **自检图太简单** — 灰色渐变图不是人体，模型合理输出 0

**自检的作用**：用已知非零的测试图排除 UVC 摄像头输入问题。如果自检也输出全零 → 模型/推理管道问题。如果自检有输出 → UVC 输入问题。

---

## 已知的 UVC 摄像头问题

CLAUDE.md 中有详细记录。ESP32-P4-Function-EV-Board 的 USB Host 对 ISOC 传输不可靠（LRCP U3-JX02 摄像头）：
- `frame error`, `invalid MJPEG SOI`, `missed EoF`
- JPEG 硬件解码超时 (`jpeg_decoder_process timeout`)
- 最终方案：迁移到 MIPI-CSI（commit `67e65f9` 有历史代码）

**但当前代码已配置 YUY2 640×480**（从日志可见），理论上不经过 JPEG 解码。UVC 错误可能来自驱动仍在尝试 MJPG。

---

## 文件清单

| 文件 | 状态 | 说明 |
|------|------|------|
| `main/model_loader/pose_estimator.hpp` | ✅ | 保持原始 API |
| `main/model_loader/pose_estimator.cpp` | ✅ | 完整实现, 含自检 |
| `main/app_main.c` | ✅ | 推理周期改 100ms, 其余不变 |
| `main/CMakeLists.txt` | ✅ | esp-dl → esp-tflite-micro + esp-nn |
| `main/idf_component.yml` | ✅ | esp-dl → esp-tflite-micro |
| `CMakeLists.txt` | ✅ | 加 esp-tflite-micro, esp-nn, -DESP_NN |
| `CLAUDE.md` | ✅ | 已更新架构说明 |
| `managed_components/.../kernels/cast.cc` | ⚠️ 已修改 | UINT8+INT64 支持 |
| `managed_components/.../kernels/floor_div.cc` | ⚠️ 已修改 | INT32 支持 |
| `managed_components/.../CMakeLists.txt` | ⚠️ 已修改 | PRIV_REQUIRES 加 esp-nn |
| `F:\models\movenet_singlepose_lightning_int8.tflite` | ✅ | SD 卡模型 (需用户拷贝) |

---

## 下一步排查方向（给下一位 AI）

### 优先级 1: 验证 ESP-NN 是否编译/链接
```cmake
# 在 esp-tflite-micro/CMakeLists.txt 中加:
message(STATUS "ESP-NN P4 sources: ${p4_srcs}")  # 但这是 esp-nn 自己的 CMakeLists
# 或在 conv.cc (esp_nn 版) 的第一行加:
#pragma message "COMPILING ESP_NN CONV"
```

### 优先级 2: 检查输出张量类型
日志显示 `Output: [1,1,17,3], type=1`。在 TFLite 运行时:
- `kTfLiteNoType = 0`
- `kTfLiteFloat32 = 1`
- `kTfLiteInt32 = 2`
- `kTfLiteUInt8 = 3`

所以 type=1 是 `kTfLiteFloat32` ✅。但如果实际上是 FLOAT16 就不对。
验证: 打印 `s_output_tensor->type` 的值并对照 `TfLiteType` 枚举。

### 优先级 3: 自检输出
等待 `run_self_test()` 输出。关键判断:
- 如果 3 个 trial 都 <100ms → ESP-NN 其实生效了！4.2s 是 UVC 问题
- 如果 3 个 trial 都 ~4s → ESP-NN 没生效
- 如果 max_conf > 0 → 模型正常，是 UVC 输入问题
- 如果 max_conf = 0 → 模型管道有问题

### 优先级 4: 调试 Conv2D 内核
```cpp
// 在 pose_estimator.cpp 中 +pose_estimator_run():
// 打印 Conv2D 操作的输入/输出 tensor 类型
// 调用 s_interpreter->GetTensorDetails() 等调试 API
```

### 优先级 5: 降级方案
如果 ESP-NN 一直不能正常工作:
- 用 ESP-DL 重新集成，用 MobileNetV2 + 自定义关键点头
- 或用 ESP-VISION 的 MicroPython tflite 模块（纯 Python，路径不同）

---

## 编译命令

```bash
# 完整重建
cd E:\esp_practice\first_practice
idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.esp32_p4_function_ev_board fullclean
idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.esp32_p4_function_ev_board build flash monitor

# 模型部署: 把 tools/movenet_singlepose_lightning_int8.tflite 复制到 SD 卡的 /models/ 目录
```

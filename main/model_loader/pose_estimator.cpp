/* ============================================================
 * pose_estimator.cpp — MoveNet SinglePose Lightning via TFLite Micro
 *
 * 替换了之前的 YOLO26n-pose + ESP-DL 方案。
 * 输入: RGB565 帧 → 192×192×3 UINT8 letterbox → Inference
 * 输出: 17 COCO 关键点，归一化坐标 [-1, 1]
 * ============================================================ */

#include "pose_estimator.hpp"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <new>
#include <cinttypes>

static const char *TAG = "MOVE";

#define MODEL_PATH "/sdcard/models/movenet_singlepose_lightning_int8.tflite"
#define TENSOR_ARENA_SIZE (2 * 1024 * 1024)  /* 2 MB PSRAM */

/* ---------- 模型常量 ---------- */
static constexpr int MODEL_INPUT_W = 192;
static constexpr int MODEL_INPUT_H = 192;
static constexpr int MODEL_INPUT_C = 3;
static constexpr int NUM_KPTS     = 17;

/* ---------- 全局状态 ---------- */
static uint8_t                      *s_model_buf   = nullptr;
static const tflite::Model          *s_model       = nullptr;
static tflite::MicroInterpreter     *s_interpreter = nullptr;
static uint8_t                      *s_tensor_arena = nullptr;

/* 算子解析器必须比 s_interpreter 活得久 */
static tflite::MicroMutableOpResolver<NUM_KPTS + 8> s_resolver;
static bool                                            s_ops_registered = false;

/* ============================================================
 * 内部工具函数
 * ============================================================ */

/** 从 SD 卡读取模型文件到 PSRAM */
static uint8_t *read_file_to_psram(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "Cannot open %s", path); return nullptr; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return nullptr; }

    uint8_t *buf = (uint8_t *)heap_caps_aligned_alloc(16, sz,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "OOM for model (%ld B)", sz);
        fclose(f);
        return nullptr;
    }
    if (fread(buf, 1, sz, f) != (size_t)sz) {
        ESP_LOGE(TAG, "Short read model file");
        heap_caps_free(buf);
        fclose(f);
        return nullptr;
    }
    fclose(f);
    ESP_LOGI(TAG, "Model: %ld B from %s", sz, path);
    *out_size = (size_t)sz;
    return buf;
}

/** RGB565 单像素 → RGB888 */
static inline void rgb565_to_888(uint16_t p, uint8_t &r, uint8_t &g, uint8_t &b)
{
    r = ((p >> 11) & 0x1f) << 3;
    g = ((p >> 5)  & 0x3f) << 2;
    b = ( p        & 0x1f) << 3;
}

/**
 * @brief RGB565 → 192×192×3 UINT8 letterbox + bilinear upscale
 *
 * 保持宽高比缩放，左右/上下对称填充 0（黑色）。
 */
static void preprocess(const uint8_t *rgb565, uint32_t w, uint32_t h,
                       uint32_t stride, uint8_t *dst)
{
    float scale = fminf((float)MODEL_INPUT_W / w, (float)MODEL_INPUT_H / h);
    int new_w   = (int)(w * scale);
    int new_h   = (int)(h * scale);
    int off_x   = (MODEL_INPUT_W - new_w) / 2;
    int off_y   = (MODEL_INPUT_H - new_h) / 2;

    /* 全部填 0（黑色 letterbox 边） */
    memset(dst, 0, MODEL_INPUT_W * MODEL_INPUT_H * MODEL_INPUT_C);

    /* 双线性插值缩放 */
    for (int dy = 0; dy < new_h; dy++) {
        float src_y = (dy + 0.5f) / new_h * h - 0.5f;
        int sy0 = (int)floorf(src_y);
        int sy1 = sy0 + 1;
        float fy = src_y - sy0;
        sy0 = sy0 < 0 ? 0 : (sy0 >= (int)h ? (int)h - 1 : sy0);
        sy1 = sy1 < 0 ? 0 : (sy1 >= (int)h ? (int)h - 1 : sy1);

        for (int dx = 0; dx < new_w; dx++) {
            float src_x = (dx + 0.5f) / new_w * w - 0.5f;
            int sx0 = (int)floorf(src_x);
            int sx1 = sx0 + 1;
            float fx = src_x - sx0;
            sx0 = sx0 < 0 ? 0 : (sx0 >= (int)w ? (int)w - 1 : sx0);
            sx1 = sx1 < 0 ? 0 : (sx1 >= (int)w ? (int)w - 1 : sx1);

            uint16_t p00 = *(const uint16_t *)(rgb565 + sy0 * stride + sx0 * 2);
            uint16_t p01 = *(const uint16_t *)(rgb565 + sy0 * stride + sx1 * 2);
            uint16_t p10 = *(const uint16_t *)(rgb565 + sy1 * stride + sx0 * 2);
            uint16_t p11 = *(const uint16_t *)(rgb565 + sy1 * stride + sx1 * 2);

            uint8_t r00, g00, b00, r01, g01, b01, r10, g10, b10, r11, g11, b11;
            rgb565_to_888(p00, r00, g00, b00);
            rgb565_to_888(p01, r01, g01, b01);
            rgb565_to_888(p10, r10, g10, b10);
            rgb565_to_888(p11, r11, g11, b11);

            uint8_t r = (uint8_t)((1-fy)*((1-fx)*r00 + fx*r01) + fy*((1-fx)*r10 + fx*r11));
            uint8_t g = (uint8_t)((1-fy)*((1-fx)*g00 + fx*g01) + fy*((1-fx)*g10 + fx*g11));
            uint8_t b = (uint8_t)((1-fy)*((1-fx)*b00 + fx*b01) + fy*((1-fx)*b10 + fx*b11));

            int idx = ((off_y + dy) * MODEL_INPUT_W + (off_x + dx)) * MODEL_INPUT_C;
            dst[idx]     = r;
            dst[idx + 1] = g;
            dst[idx + 2] = b;
        }
    }
}

/* ============================================================
 * 注册 MoveNet 所需要的全部算子（共 19 种）
 * ============================================================ */
static bool register_all_ops(void)
{
    /* clang-format off */
    auto r = [](TfLiteStatus s, const char *name) {
        if (s != kTfLiteOk) ESP_LOGE(TAG, "Op registration FAILED: %s", name);
        return s == kTfLiteOk;
    };
    /* clang-format on */

    bool ok = true;
    ok &= r(s_resolver.AddConv2D(),            "CONV_2D");
    ok &= r(s_resolver.AddDepthwiseConv2D(),   "DEPTHWISE_CONV_2D");
    ok &= r(s_resolver.AddAdd(),               "ADD");
    ok &= r(s_resolver.AddReshape(),           "RESHAPE");
    ok &= r(s_resolver.AddQuantize(),          "QUANTIZE");
    ok &= r(s_resolver.AddMul(),               "MUL");
    ok &= r(s_resolver.AddSub(),               "SUB");
    ok &= r(s_resolver.AddCast(),              "CAST");
    ok &= r(s_resolver.AddDequantize(),        "DEQUANTIZE");
    ok &= r(s_resolver.AddGatherNd(),          "GATHER_ND");
    ok &= r(s_resolver.AddResizeBilinear(),    "RESIZE_BILINEAR");
    ok &= r(s_resolver.AddPack(),              "PACK");
    ok &= r(s_resolver.AddLogistic(),          "LOGISTIC");
    ok &= r(s_resolver.AddArgMax(),            "ARG_MAX");
    ok &= r(s_resolver.AddFloorDiv(),          "FLOOR_DIV");
    ok &= r(s_resolver.AddUnpack(),            "UNPACK");
    ok &= r(s_resolver.AddConcatenation(),     "CONCATENATION");
    ok &= r(s_resolver.AddSqrt(),              "SQRT");
    ok &= r(s_resolver.AddDiv(),               "DIV");
    return ok;
}

/* ============================================================
 * 公开 API
 * ============================================================ */

void pose_estimator_load_and_print(void)
{
    ESP_LOGI(TAG, "MoveNet loading...");

    /* ---- 注册算子（仅首次） ---- */
    if (!s_ops_registered) {
        if (!register_all_ops()) {
            ESP_LOGE(TAG, "Op registration failed — aborting");
            return;
        }
        s_ops_registered = true;
    }

    /* ---- 读取模型 ---- */
    size_t sz = 0;
    s_model_buf = read_file_to_psram(MODEL_PATH, &sz);
    if (!s_model_buf) return;

    /* ---- 验证 FlatBuffer ---- */
    s_model = tflite::GetModel(s_model_buf);
    if (s_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Schema version mismatch: model=%d != supported=%d",
                 s_model->version(), TFLITE_SCHEMA_VERSION);
        goto fail_free_buf;
    }

    /* ---- 分配张量竞技场 (PSRAM) ---- */
    s_tensor_arena = (uint8_t *)heap_caps_aligned_alloc(
        16, TENSOR_ARENA_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_CACHE_ALIGNED);
    if (!s_tensor_arena) {
        ESP_LOGE(TAG, "Tensor arena OOM (%d B)", TENSOR_ARENA_SIZE);
        goto fail_free_buf;
    }
    ESP_LOGI(TAG, "Tensor arena: %d B in PSRAM", TENSOR_ARENA_SIZE);

    /* ---- 创建解释器 ---- */
    s_interpreter = new (std::nothrow) tflite::MicroInterpreter(
        s_model, s_resolver, s_tensor_arena, TENSOR_ARENA_SIZE);
    if (!s_interpreter) {
        ESP_LOGE(TAG, "MicroInterpreter allocation failed");
        goto fail_free_all;
    }

    /* ---- 分配张量 ---- */
    if (s_interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed");
        goto fail_free_all;
    }

    /* ---- 打印信息（已通过 AllocateTensors，可以安全使用指针） ---- */
    {
        TfLiteTensor *in  = s_interpreter->input(0);
        TfLiteTensor *out = s_interpreter->output(0);
        ESP_LOGI(TAG, "MoveNet ready!");
        ESP_LOGI(TAG, "  Input:  [%d,%d,%d,%d] type=%d q=(%f,%d)",
                 in->dims->data[0], in->dims->data[1],
                 in->dims->data[2], in->dims->data[3],
                 in->type, in->params.scale, in->params.zero_point);
        ESP_LOGI(TAG, "  Output: [%d,%d,%d,%d] type=%d",
                 out->dims->data[0], out->dims->data[1],
                 out->dims->data[2], out->dims->data[3],
                 out->type);
    }
    return;

fail_free_all:
    delete s_interpreter;
    s_interpreter = nullptr;
    heap_caps_free(s_tensor_arena);
    s_tensor_arena = nullptr;
fail_free_buf:
    heap_caps_free(s_model_buf);
    s_model_buf  = nullptr;
    s_model      = nullptr;
}

esp_err_t pose_estimator_run(const uint8_t *rgb565_buf,
                              uint32_t w, uint32_t h, uint32_t stride,
                              float joints_out[17][2],
                              float confidences[17],
                              float *score_out)
{
    if (!s_interpreter) {
        ESP_LOGE(TAG, "Model not loaded");
        return ESP_ERR_INVALID_STATE;
    }
    if (!rgb565_buf || w == 0 || h == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t t0 = esp_timer_get_time();

    /* ---- Step 1: 预处理 ---- */
    TfLiteTensor *input = s_interpreter->input(0);
    preprocess(rgb565_buf, w, h, stride, input->data.uint8);

    int64_t t1 = esp_timer_get_time();

    /* ---- Step 2: 推理 ---- */
    if (s_interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Invoke failed");
        return ESP_FAIL;
    }

    int64_t t2 = esp_timer_get_time();

    /* ---- Step 3: 后处理 ---- */
    /* MoveNet output: [1, 1, 17, 3] FLOAT32
     * 每个关键点 [y, x, score]  归一化 [0, 1] */
    TfLiteTensor *output = s_interpreter->output(0);
    float *out = output->data.f;

    int    valid    = 0;
    float  max_s    = 0.0f;
    for (int i = 0; i < NUM_KPTS; i++) {
        float y = out[i * 3];
        float x = out[i * 3 + 1];
        float s = out[i * 3 + 2];

        if (s > 0.1f) {
            /* [0, 1] → [-1, 1]（UI 约定） */
            joints_out[i][0] = x * 2.0f - 1.0f;
            joints_out[i][1] = y * 2.0f - 1.0f;
            confidences[i]   = s;
            valid++;
        } else {
            joints_out[i][0] = 0.0f;
            joints_out[i][1] = 0.0f;
            confidences[i]   = 0.0f;
        }
        if (s > max_s) max_s = s;
    }

    if (score_out) *score_out = max_s;

    int64_t t3 = esp_timer_get_time();
    ESP_LOGI(TAG, "pre=%" PRId64 "ms infer=%" PRId64 "ms post=%" PRId64 "ms total=%" PRId64 "ms  kpts=%d/17 score=%.3f",
             (t1 - t0) / 1000, (t2 - t1) / 1000,
             (t3 - t2) / 1000, (t3 - t0) / 1000,
             valid, max_s);
    if (valid > 0) {
        float *out_data = s_interpreter->output(0)->data.f;
        ESP_LOGI(TAG, "  nose=(%.2f,%.2f,%.2f)  l_ear=(%.2f,%.2f)  l_eye=(%.2f,%.2f)",
                 out_data[0], out_data[1], out_data[2], out_data[3], out_data[4], out_data[6], out_data[7]);
    }

    return (valid > 0) ? ESP_OK : ESP_FAIL;
}

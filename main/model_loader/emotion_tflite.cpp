/* ============================================================
 * emotion_tflite.cpp — EmotionCNN via TFLite Micro
 *
 * Model: /sdcard/models/emotion_cnn_aug.tflite (~80KB INT8)
 * Input:  48×48×1 UINT8 grayscale
 * Output: 7-class FLOAT32 logits → softmax → argmax
 * ============================================================ */

#include "emotion_tflite.hpp"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <sys/stat.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "EMO_TFL";

#define MODEL_PATH        "/sdcard/models/emotion_cnn_aug.tflite"
#define TENSOR_ARENA_SIZE (2 * 1024 * 1024)   /* 2 MB — float32 needs room for activations */
#define INPUT_SIZE        48
#define NUM_CLASSES       7

static const char *EMOTION_NAMES[] = {
    "Angry", "Disgust", "Fear", "Happy", "Sad", "Surprise", "Neutral"
};

static uint8_t *s_arena = nullptr;
static const tflite::Model *s_tflite_model = nullptr;
static tflite::MicroInterpreter *s_interpreter = nullptr;
static TfLiteTensor *s_input = nullptr;
static TfLiteTensor *s_output = nullptr;

/* ---- Load model from SD card ---- */
void emotion_tflite_load(void)
{
    struct stat st;
    if (stat(MODEL_PATH, &st) != 0) {
        ESP_LOGW(TAG, "Model not found: %s", MODEL_PATH);
        return;
    }

    /* Read entire model file into PSRAM */
    FILE *fp = fopen(MODEL_PATH, "rb");
    if (!fp) {
        ESP_LOGW(TAG, "Cannot open %s", MODEL_PATH);
        return;
    }
    size_t model_size = st.st_size;
    uint8_t *model_data = (uint8_t *)heap_caps_aligned_alloc(
        16, model_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!model_data) {
        ESP_LOGE(TAG, "Model alloc failed (%u bytes)", (unsigned)model_size);
        fclose(fp);
        return;
    }
    fread(model_data, 1, model_size, fp);
    fclose(fp);

    s_tflite_model = tflite::GetModel(model_data);
    if (s_tflite_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Schema version mismatch: model=%u runtime=%u",
                 s_tflite_model->version(), TFLITE_SCHEMA_VERSION);
        heap_caps_free(model_data);
        s_tflite_model = nullptr;
        return;
    }

    /* Allocate tensor arena */
    s_arena = (uint8_t *)heap_caps_aligned_alloc(
        16, TENSOR_ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!s_arena) {
        ESP_LOGE(TAG, "Arena alloc failed");
        heap_caps_free(model_data);
        s_tflite_model = nullptr;
        return;
    }

    /* Register ops */
    static tflite::MicroMutableOpResolver<8> resolver;
    resolver.AddConv2D();               /* Conv 3×3 + 1×1 */
    resolver.AddMaxPool2D();            /* Downsample */
    resolver.AddAdd();                  /* Residual connections */
    resolver.AddMean();                 /* AdaptiveAvgPool2d */
    resolver.AddReshape();              /* Flatten */
    resolver.AddFullyConnected();       /* Linear classifier */
    resolver.AddSoftmax();              /* Output */
    resolver.AddMul();                  /* BN scale */

    /* Build interpreter */
    s_interpreter = new (std::nothrow) tflite::MicroInterpreter(
        s_tflite_model, resolver, s_arena, TENSOR_ARENA_SIZE);
    if (!s_interpreter) {
        ESP_LOGE(TAG, "Interpreter alloc failed");
        return;
    }

    if (s_interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed");
        delete s_interpreter;
        s_interpreter = nullptr;
        return;
    }

    s_input  = s_interpreter->input(0);
    s_output = s_interpreter->output(0);

    ESP_LOGI(TAG, "TFLite ready. Input: %dx%dx%d (%s), Output: %d classes",
             s_input->dims->data[1], s_input->dims->data[2],
             s_input->dims->data[3],
             s_input->type == kTfLiteInt8 ? "INT8" : "FLOAT32",
             s_output->dims->data[1]);
}

/* ---- Preprocess: RGB565 ROI → grayscale 48×48 UINT8 ---- */
static bool preprocess_roi(const uint8_t *rgb565,
                           int frame_w, int frame_h, int stride,
                           int fx, int fy, int fw, int fh)
{
    if (!s_input) return false;

    float *dst = s_input->data.f;
    int in_h = s_input->dims->data[1];  /* 48 */
    int in_w = s_input->dims->data[2];  /* 48 */

    /* Clamp ROI */
    int x0 = (fx < 0) ? 0 : fx;
    int y0 = (fy < 0) ? 0 : fy;
    int x1 = (fx + fw > frame_w) ? frame_w : fx + fw;
    int y1 = (fy + fh > frame_h) ? frame_h : fy + fh;
    int crop_w = x1 - x0;
    int crop_h = y1 - y0;

    if (crop_w < 4 || crop_h < 4) {
        memset(dst, 0, in_h * in_w * sizeof(float));
        return true;
    }

    /* Nearest-neighbor resize + RGB565→grayscale + INT8 quant (zero point -128) */
    for (int y = 0; y < in_h; y++) {
        int src_y = y0 + y * crop_h / in_h;
        if (src_y >= frame_h) src_y = frame_h - 1;

        for (int x = 0; x < in_w; x++) {
            int src_x = x0 + x * crop_w / in_w;
            if (src_x >= frame_w) src_x = frame_w - 1;

            int idx = src_y * stride + src_x * 2;
            uint16_t px = rgb565[idx] | (rgb565[idx + 1] << 8);
            int r = ((px >> 11) & 0x1F) << 3;
            int g = ((px >> 5)  & 0x3F) << 2;
            int b = ( px        & 0x1F) << 3;
            int gray = (r * 77 + g * 150 + b * 29) >> 8;

            /* FLOAT32: gray [0,255] → [0.0, 1.0] */
            dst[y * in_w + x] = (float)gray / 255.0f;
        }
    }
    return true;
}

/* ---- Run inference ---- */
esp_err_t emotion_tflite_run(const uint8_t *rgb565,
                             int frame_w, int frame_h, int stride,
                             int face_x, int face_y, int face_w, int face_h,
                             int *emotion_class, float *confidence)
{
    if (!s_interpreter || !s_input || !rgb565) return ESP_FAIL;

    if (!preprocess_roi(rgb565, frame_w, frame_h, stride,
                        face_x, face_y, face_w, face_h))
        return ESP_FAIL;

    if (s_interpreter->Invoke() != kTfLiteOk) {
        return ESP_FAIL;
    }

    /* Output: FLOAT32 logits → softmax → argmax */
    float *logits = s_output->data.f;
    int n_out = s_output->dims->data[1];
    if (n_out < NUM_CLASSES) n_out = NUM_CLASSES;

    float max_val = logits[0];
    for (int i = 1; i < n_out; i++)
        if (logits[i] > max_val) max_val = logits[i];

    float sum = 0.0f;
    float probs[NUM_CLASSES];
    for (int i = 0; i < NUM_CLASSES; i++) {
        probs[i] = expf(logits[i] - max_val);
        sum += probs[i];
    }

    int best = 0;
    float best_conf = 0.0f;
    for (int i = 0; i < NUM_CLASSES; i++) {
        probs[i] /= sum;
        if (probs[i] > best_conf) { best_conf = probs[i]; best = i; }
    }

    *emotion_class = best;
    *confidence = best_conf;

    ESP_LOGI(TAG, "%s (%.1f%%)", EMOTION_NAMES[best], (double)(best_conf * 100.0f));
    return ESP_OK;
}

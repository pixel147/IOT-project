/* ============================================================
 * emotion_tflite.cpp — EmotionCNN via TFLite Micro (INT8)
 *
 * Model: /sdcard/models/emotion_cnn_aug_int8.tflite
 * Input:  48×48×1 UINT8 grayscale → INT8 quantization inside model
 * Output: 7-class FLOAT32 logits → softmax → argmax
 *
 * INT8 model uses ESP-NN accelerated convolutions → ~10-30ms
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

#define MODEL_PATH        "/sdcard/models/emotion_cnn_aug_int8.tflite"
#define TENSOR_ARENA_SIZE (512 * 1024)     /* INT8: 512KB足够 */
#define INPUT_SIZE        48
#define NUM_CLASSES       7

static const char *EMOTION_NAMES[] = {
    "Angry", "Disgust", "Fear", "Happy", "Sad", "Surprise", "Neutral"
};

static uint8_t *s_model_data = nullptr;     /* 模型文件缓冲（必须保持有效） */
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

    FILE *fp = fopen(MODEL_PATH, "rb");
    if (!fp) { ESP_LOGW(TAG, "Cannot open %s", MODEL_PATH); return; }
    size_t model_size = st.st_size;
    s_model_data = (uint8_t *)heap_caps_aligned_alloc(
        16, model_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!s_model_data) { ESP_LOGE(TAG, "Model alloc fail"); fclose(fp); return; }
    fread(s_model_data, 1, model_size, fp);
    fclose(fp);

    s_tflite_model = tflite::GetModel(s_model_data);
    if (s_tflite_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Schema version mismatch");
        heap_caps_free(s_model_data); s_model_data = nullptr; return;
    }

    s_arena = (uint8_t *)heap_caps_aligned_alloc(
        16, TENSOR_ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!s_arena) { ESP_LOGE(TAG, "Arena OOM"); heap_caps_free(s_model_data); return; }

    /* INT8 model ops: convolution uses ESP-NN accelerated INT8 kernels */
    static tflite::MicroMutableOpResolver<12> resolver;
    resolver.AddConv2D();
    resolver.AddMaxPool2D();
    resolver.AddAdd();
    resolver.AddQuantize();          /* UINT8 input → INT8 internal */
    resolver.AddDequantize();        /* INT8 → FLOAT32 output */
    resolver.AddMean();              /* GlobalAveragePool */
    resolver.AddFullyConnected();
    resolver.AddReshape();

    s_interpreter = new (std::nothrow) tflite::MicroInterpreter(
        s_tflite_model, resolver, s_arena, TENSOR_ARENA_SIZE);
    if (!s_interpreter) return;
    if (s_interpreter->AllocateTensors() != kTfLiteOk) {
        delete s_interpreter; s_interpreter = nullptr; return;
    }

    s_input  = s_interpreter->input(0);
    s_output = s_interpreter->output(0);
    ESP_LOGI(TAG, "INT8 ready. Input: %dx%dx%d type=%d q=(%f,%d) Arena=%dKB",
             s_input->dims->data[1], s_input->dims->data[2], s_input->dims->data[3],
             s_input->type, s_input->params.scale, s_input->params.zero_point,
             TENSOR_ARENA_SIZE / 1024);
}

/* ---- Preprocess: RGB565 ROI → grayscale 48×48 UINT8 ---- */
static bool preprocess_roi(const uint8_t *rgb565,
                           int frame_w, int frame_h, int stride,
                           int fx, int fy, int fw, int fh)
{
    if (!s_input) return false;
    uint8_t *dst = s_input->data.uint8;      /* UINT8 input */
    int in_h = s_input->dims->data[1];       /* 48 */
    int in_w = s_input->dims->data[2];       /* 48 */

    int x0 = fx < 0 ? 0 : fx;
    int y0 = fy < 0 ? 0 : fy;
    int x1 = fx + fw > frame_w ? frame_w : fx + fw;
    int y1 = fy + fh > frame_h ? frame_h : fy + fh;
    int crop_w = x1 - x0, crop_h = y1 - y0;

    if (crop_w < 4 || crop_h < 4) {
        memset(dst, 0, in_h * in_w);         /* 黑填充 */
        return true;
    }

    for (int y = 0; y < in_h; y++) {
        int src_y = y0 + y * crop_h / in_h;
        if (src_y >= frame_h) src_y = frame_h - 1;
        for (int x = 0; x < in_w; x++) {
            int src_x = x0 + x * crop_w / in_w;
            if (src_x >= frame_w) src_x = frame_w - 1;
            int idx = src_y * stride + src_x * 2;
            uint16_t px = rgb565[idx] | (rgb565[idx + 1] << 8);
            int gray = ((((px >> 11) & 0x1F) << 3) * 77 +
                        (((px >> 5)  & 0x3F) << 2) * 150 +
                        (((px)       & 0x1F) << 3) * 29) >> 8;
            dst[y * in_w + x] = (uint8_t)gray;  /* UINT8 直写 [0,255] */
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

    if (s_interpreter->Invoke() != kTfLiteOk) return ESP_FAIL;

    /* Output: FLOAT32 logits */
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

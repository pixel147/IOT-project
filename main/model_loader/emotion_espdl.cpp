/* ============================================================
 * emotion_espdl.cpp — EmotionCNN via ESP-DL (INT8 per-tensor)
 *
 * Model: /sdcard/models/emotion_cnn_aug_pt.espdl (101 KB)
 * Input:  48×48×1 INT8 grayscale, exponent=-7 (scale=1/128)
 * Output: 7-class INT8 logits, exponent=-3 → softmax → argmax
 *
 * ESP-DL uses P4 internal PPA/NN cores → ~2-8ms inference
 * ============================================================ */

#include "emotion_espdl.hpp"

#include "esp_log.h"
#include "esp_cache.h"

#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"
#include "fbs_loader.hpp"

#include <cmath>
#include <cstring>
#include <new>

static const char *TAG = "EMO_ESPDL";

#define MODEL_PATH        "/sdcard/models/emotion_cnn_aug_pt.espdl"
#define INPUT_SIZE        48
#define NUM_CLASSES       7

static dl::Model *s_model = nullptr;

static const char *EMOTION_NAMES[] = {
    "Angry", "Disgust", "Fear", "Happy", "Sad", "Surprise", "Neutral"
};

/* ============================================================
 * Load model from SD card
 * ============================================================ */
void emotion_espdl_load(void)
{
    ESP_LOGI(TAG, "Loading ESP-DL model from %s", MODEL_PATH);

    s_model = new (std::nothrow) dl::Model(
        MODEL_PATH,
        fbs::MODEL_LOCATION_IN_SDCARD,
        0,                          /* internal_size: 0 = auto (PSRAM) */
        dl::MEMORY_MANAGER_GREEDY);

    if (!s_model) {
        ESP_LOGE(TAG, "Failed to create Model object (OOM)");
        return;
    }

    /* 验证模型加载是否正确：get_inputs() 必须非空 */
    auto &inputs = s_model->get_inputs();
    if (inputs.empty()) {
        ESP_LOGE(TAG, "Model loaded but has NO inputs! File may be missing or corrupt.");
        ESP_LOGE(TAG, "Expected: %s (101 KB)", MODEL_PATH);
        delete s_model; s_model = nullptr;
        return;
    }
    ESP_LOGI(TAG, "Model inputs: %zu", inputs.size());
    for (auto &kv : inputs) {
        auto *t = kv.second;
        ESP_LOGI(TAG, "  Input '%s': shape [%d x %d x %d x %d], dtype=%d, exponent=%d",
                 kv.first.c_str(),
                 t->shape[0], t->shape[1], t->shape[2], t->shape[3],
                 t->dtype, t->exponent);
    }

    auto &outputs = s_model->get_outputs();
    if (outputs.empty()) {
        ESP_LOGE(TAG, "Model loaded but has NO outputs!");
        delete s_model; s_model = nullptr;
        return;
    }
    ESP_LOGI(TAG, "Model outputs: %zu", outputs.size());
    for (auto &kv : outputs) {
        auto *t = kv.second;
        std::string shape_str;
        for (size_t i = 0; i < t->shape.size(); i++) {
            char buf[16]; snprintf(buf, sizeof(buf), "%d", t->shape[i]);
            shape_str += buf;
            if (i < t->shape.size() - 1) shape_str += "x";
        }
        ESP_LOGI(TAG, "  Output '%s': shape [%s], dtype=%d, exponent=%d",
                 kv.first.c_str(), shape_str.c_str(), t->dtype, t->exponent);
    }

    ESP_LOGI(TAG, "ESP-DL emotion model ready (expected <10ms inference)");
}

/* ============================================================
 * Preprocess: RGB565 face ROI -> 48x48 INT8 grayscale
 *
 * Model input: INT8 [-128..127], exponent=-7 (scale=1/128)
 * Mapping: pixel [0..255] -> gray -> gray - 128 -> [-128..127]  (零中心化, 满量程利用INT8)
 * ============================================================ */
static bool preprocess_roi(const uint8_t *rgb565,
                           int frame_w, int frame_h, int stride,
                           int fx, int fy, int fw, int fh)
{
    if (!s_model) return false;

    auto &inputs = s_model->get_inputs();
    if (inputs.empty()) return false;

    dl::TensorBase *input_tensor = inputs.begin()->second;
    if (!input_tensor || !input_tensor->data) {
        ESP_LOGW(TAG, "Input tensor not allocated");
        return false;
    }

    int8_t *dst = static_cast<int8_t *>(input_tensor->data);
    const int in_h = INPUT_SIZE;
    const int in_w = INPUT_SIZE;

    /* Clamp face ROI to frame bounds */
    int x0 = (fx < 0) ? 0 : fx;
    int y0 = (fy < 0) ? 0 : fy;
    int x1 = (fx + fw > frame_w) ? frame_w : fx + fw;
    int y1 = (fy + fh > frame_h) ? frame_h : fy + fh;
    int crop_w = x1 - x0;
    int crop_h = y1 - y0;

    if (crop_w < 4 || crop_h < 4) {
        /* Too small — fill black (gray=0 -> int8=0) */
        memset(dst, 0, (size_t)in_h * in_w);
        return true;
    }

    /* Bilinear-like resize via nearest-neighbor with sub-pixel stepping.
     * Converts RGB565 -> grayscale -> center at 0 (full INT8 range). */
    for (int y = 0; y < in_h; y++) {
        int src_y = y0 + (y * crop_h) / in_h;
        if (src_y >= frame_h) src_y = frame_h - 1;
        const uint8_t *row = rgb565 + src_y * stride;

        for (int x = 0; x < in_w; x++) {
            int src_x = x0 + (x * crop_w) / in_w;
            if (src_x >= frame_w) src_x = frame_w - 1;

            int idx = src_x * 2;
            uint16_t px = row[idx] | ((uint16_t)row[idx + 1] << 8);

            /* Grayscale: 0.299R + 0.587G + 0.114B (fixed-point) */
            int r = (px >> 11) & 0x1F;
            int g = (px >> 5)  & 0x3F;
            int b =  px        & 0x1F;
            int gray = (r * 77 + g * 150 + b * 29) >> 6;  /* [0..255] */

            /* Center at 0 for full INT8 range: gray [0,255] -> [-128, 127]
             * With exponent=-7 (scale=1/128): real = int8 * 2^-7 = [-1.0, 0.992] */
            dst[y * in_w + x] = (int8_t)(gray - 128);
        }
    }

    /* Flush cache so NN accelerator sees the data */
    size_t align_sz = ((size_t)in_h * in_w + 63) & ~63;
    esp_cache_msync(dst, align_sz, ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

    return true;
}

/* ============================================================
 * Softmax on dequantized INT8 logits
 * ============================================================ */
static void softmax_deq(const int8_t *logits, int n, float *probs, int exponent)
{
    float scale = exp2f((float)exponent);  /* 2^exponent */
    float max_val = (float)logits[0] * scale;
    for (int i = 1; i < n; i++) {
        float v = (float)logits[i] * scale;
        if (v > max_val) max_val = v;
    }

    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        probs[i] = expf((float)logits[i] * scale - max_val);
        sum += probs[i];
    }
    float inv_sum = 1.0f / sum;
    for (int i = 0; i < n; i++) probs[i] *= inv_sum;
}

/* ============================================================
 * Run inference
 * ============================================================ */
esp_err_t emotion_espdl_run(const uint8_t *rgb565,
                             int frame_w, int frame_h, int stride,
                             int face_x, int face_y, int face_w, int face_h,
                             int *emotion_class, float *confidence)
{
    if (!s_model || !rgb565 || !emotion_class || !confidence)
        return ESP_ERR_INVALID_ARG;

    if (!preprocess_roi(rgb565, frame_w, frame_h, stride,
                        face_x, face_y, face_w, face_h))
        return ESP_FAIL;

    /* Run the full model graph */
    s_model->run();

    /* Read output tensor */
    auto &outputs = s_model->get_outputs();
    if (outputs.empty()) {
        ESP_LOGW(TAG, "No model outputs");
        return ESP_FAIL;
    }

    dl::TensorBase *out_tensor = outputs.begin()->second;
    if (!out_tensor || !out_tensor->data) return ESP_FAIL;

    /* Invalidate cache before CPU reads output (written by accelerator) */
    size_t out_bytes = (size_t)out_tensor->get_bytes();
    size_t align_sz = (out_bytes + 63) & ~63;
    if (align_sz > 0) {
        esp_cache_msync(out_tensor->data, align_sz, ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    }

    int8_t *logits = static_cast<int8_t *>(out_tensor->data);
    int out_exponent = out_tensor->exponent;  /* expected -3 */

    /* Get number of classes from output shape (last dim) */
    int n_out = (!out_tensor->shape.empty())
                    ? out_tensor->shape.back()
                    : NUM_CLASSES;
    if (n_out != NUM_CLASSES) n_out = NUM_CLASSES;

    /* Dequantize + softmax */
    float probs[NUM_CLASSES];
    softmax_deq(logits, n_out, probs, out_exponent);

    int best = 0;
    float best_conf = probs[0];
    for (int i = 1; i < n_out; i++) {
        if (probs[i] > best_conf) {
            best_conf = probs[i];
            best = i;
        }
    }

    *emotion_class = best;
    *confidence = best_conf;

    ESP_LOGI(TAG, "%s (%.1f%%) [%d %d %d %d %d %d %d]",
             EMOTION_NAMES[best], (double)(best_conf * 100.0f),
             logits[0], logits[1], logits[2],
             logits[3], logits[4], logits[5], logits[6]);

    return ESP_OK;
}

/* ============================================================
 * emotion_estimator.cpp — EmotionCNN 情绪识别推理
 *
 * 模型: emotion_cnn_aug.espdl (INT8, 76,855 参数)
 * 输入: [1, 48, 48, 1] (NHWC, 灰度, INT8)
 * 输出: [1, 7] (7 类 logits, INT8)
 *
 * 预处理: RGB565 面部 ROI → 灰度 → 48×48 resize → INT8 量化
 * 后处理: INT8 logits → float → softmax → argmax
 * ============================================================ */

#include "emotion_estimator.hpp"

#include "dl_model_base.hpp"
#include "fbs_model.hpp"
#include "dl_tensor_base.hpp"

#include "esp_log.h"
#include <cstdio>
#include <cmath>
#include <cstring>

static const char *TAG = "EMOTION";

#define MODEL_PATH        "/sdcard/models/emotion_cnn_aug.espdl"
#define EMOTION_IN_SIZE   48
#define NUM_CLASSES       7

static const char *EMOTION_NAMES[] = {
    "Angry", "Disgust", "Fear", "Happy", "Sad", "Surprise", "Neutral"
};

static dl::Model *s_emotion_model = nullptr;

/* ---- 模型加载 ---- */
void emotion_estimator_load(void)
{
    ESP_LOGI(TAG, "Loading EmotionCNN: %s", MODEL_PATH);

    s_emotion_model = new (std::nothrow) dl::Model(
        MODEL_PATH, fbs::MODEL_LOCATION_IN_SDCARD,
        0, dl::MEMORY_MANAGER_GREEDY, nullptr, true);

    if (!s_emotion_model || !s_emotion_model->get_fbs_model()) {
        ESP_LOGE(TAG, "Model load FAILED");
        delete s_emotion_model;
        s_emotion_model = nullptr;
        return;
    }

    ESP_LOGI(TAG, "Model loaded.");
    s_emotion_model->print();

    /* 打印模型输入/输出信息 */
    dl::TensorBase *inp = s_emotion_model->get_input();
    if (inp) {
        ESP_LOGI(TAG, "Input:  shape=[%d,%d,%d,%d] dtype=%d exp=%d",
                 inp->shape[0], inp->shape[1], inp->shape[2], inp->shape[3],
                 (int)inp->dtype, inp->get_exponent());
    }

    auto &outputs = s_emotion_model->get_outputs();
    for (auto &kv : outputs) {
        dl::TensorBase *t = kv.second;
        ESP_LOGI(TAG, "Output: %s shape=[%d,%d,%d,%d] dtype=%d exp=%d",
                 kv.first.c_str(),
                 t->shape.size() > 0 ? t->shape[0] : -1,
                 t->shape.size() > 1 ? t->shape[1] : -1,
                 t->shape.size() > 2 ? t->shape[2] : -1,
                 t->shape.size() > 3 ? t->shape[3] : -1,
                 (int)t->dtype, t->get_exponent());
    }

    ESP_LOGI(TAG, "EmotionCNN ready.");
}

/* ---- 预处理: RGB565 face ROI → grayscale → 48×48 INT8 ---- */
static bool preprocess_face(const uint8_t *rgb565_buf,
                            uint32_t frame_w, uint32_t frame_h,
                            uint32_t stride,
                            int fx, int fy, int fw, int fh)
{
    dl::TensorBase *input = s_emotion_model->get_input();
    if (!input) return false;

    int out_h = input->shape[1];  // 48
    int out_w = input->shape[2];  // 48
    int out_c = input->shape[3];  // 1
    int inp_exp = input->get_exponent();
    float inp_scale = powf(2.0f, (float)inp_exp);

    int8_t *dst = reinterpret_cast<int8_t *>(input->get_element_ptr());
    if (!dst) return false;

    /* clamp face ROI to frame bounds */
    int x0 = (fx < 0) ? 0 : fx;
    int y0 = (fy < 0) ? 0 : fy;
    int x1 = (fx + fw > (int)frame_w) ? (int)frame_w : fx + fw;
    int y1 = (fy + fh > (int)frame_h) ? (int)frame_h : fy + fh;
    int crop_w = x1 - x0;
    int crop_h = y1 - y0;

    if (crop_w < 4 || crop_h < 4) {
        /* face too small — fill with neutral gray (128 → INT8 64/scale) */
        int8_t gray_val = (int8_t)(128.0f / inp_scale);
        memset(dst, gray_val, out_h * out_w * out_c);
        return true;
    }

    /* INT8 quantize LUT: grayscale [0,255] → INT8 = round(gray / scale) */
    int divisor = (int)inp_scale;
    if (divisor < 1) divisor = 1;
    int8_t gray_lut[256];
    int i8_max = 127, i8_min = -128;
    for (int i = 0; i < 256; i++) {
        int v = (i + divisor / 2) / divisor;
        gray_lut[i] = (int8_t)(v > i8_max ? i8_max : (v < i8_min ? i8_min : v));
    }

    /* nearest-neighbor resize + grayscale conversion
     * RGB565 → gray: Y = 0.299R + 0.587G + 0.114B
     * 用整数近似: Y = (R*77 + G*150 + B*29) >> 8 */
    for (int y = 0; y < out_h; y++) {
        int src_y = y0 + y * crop_h / out_h;
        if (src_y >= (int)frame_h) src_y = frame_h - 1;
        int dst_row = y * out_w * out_c;

        for (int x = 0; x < out_w; x++) {
            int src_x = x0 + x * crop_w / out_w;
            if (src_x >= (int)frame_w) src_x = frame_w - 1;

            int src_idx = src_y * (int)stride + src_x * 2;
            uint16_t px = rgb565_buf[src_idx] | (rgb565_buf[src_idx + 1] << 8);

            int r = (px >> 11) & 0x1F;
            int g = (px >> 5)  & 0x3F;
            int b =  px        & 0x1F;
            /* expand to 8-bit */
            r = (r << 3) | (r >> 2);
            g = (g << 2) | (g >> 4);
            b = (b << 3) | (b >> 2);
            /* RGB → grayscale */
            int gray = (r * 77 + g * 150 + b * 29) >> 8;

            dst[dst_row + x * out_c] = gray_lut[gray];
        }
    }

    return true;
}

/* ---- 后处理: INT8 logits → float → softmax → argmax ---- */
static bool decode_emotion(int *emotion_class, float *confidence)
{
    auto &outputs = s_emotion_model->get_outputs();
    if (outputs.empty()) {
        ESP_LOGE(TAG, "No model outputs");
        return false;
    }

    dl::TensorBase *logits = outputs.begin()->second;
    if (!logits) return false;

    int out_exp = logits->get_exponent();
    float out_scale = powf(2.0f, -(float)out_exp);

    bool is_int8 = (logits->dtype == dl::DATA_TYPE_INT8);
    int n_out = 1;
    for (size_t i = 0; i < logits->shape.size(); i++)
        n_out *= logits->shape[i];
    if (n_out < NUM_CLASSES) n_out = NUM_CLASSES;

    /* 反量化 + 数值稳定 softmax */
    float raw[7];
    float max_val = -1e9f;
    for (int i = 0; i < NUM_CLASSES && i < n_out; i++) {
        if (is_int8) {
            raw[i] = (float)((int8_t *)logits->get_element_ptr())[i] * out_scale;
        } else {
            raw[i] = ((float *)logits->get_element_ptr())[i];
        }
        if (raw[i] > max_val) max_val = raw[i];
    }

    float sum_exp = 0.0f;
    float probs[7];
    for (int i = 0; i < NUM_CLASSES; i++) {
        probs[i] = expf(raw[i] - max_val);
        sum_exp += probs[i];
    }

    int best = 0;
    float best_conf = 0.0f;
    for (int i = 0; i < NUM_CLASSES; i++) {
        probs[i] /= sum_exp;
        if (probs[i] > best_conf) {
            best_conf = probs[i];
            best = i;
        }
    }

    *emotion_class = best;
    *confidence = best_conf;

    ESP_LOGI(TAG, "Emotion: %s (%.1f%%)",
             EMOTION_NAMES[best], (double)(best_conf * 100.0f));

#if CONFIG_LOG_DEFAULT_LEVEL >= 4  /* DEBUG */
    for (int i = 0; i < NUM_CLASSES; i++) {
        ESP_LOGD(TAG, "  %s: %.1f%%", EMOTION_NAMES[i], (double)(probs[i] * 100.0f));
    }
#endif

    return true;
}

/* ---- 推理入口 ---- */
esp_err_t emotion_estimator_run(const uint8_t *rgb565_buf,
                                uint32_t frame_w, uint32_t frame_h,
                                uint32_t frame_stride,
                                int face_x, int face_y,
                                int face_w, int face_h,
                                int *emotion_class,
                                float *confidence)
{
    if (!s_emotion_model) return ESP_ERR_INVALID_STATE;
    if (!rgb565_buf || frame_w == 0 || frame_h == 0) return ESP_ERR_INVALID_ARG;
    if (!emotion_class || !confidence) return ESP_ERR_INVALID_ARG;

    if (!preprocess_face(rgb565_buf, frame_w, frame_h, frame_stride,
                         face_x, face_y, face_w, face_h))
        return ESP_FAIL;

    s_emotion_model->run();

    if (!decode_emotion(emotion_class, confidence))
        return ESP_FAIL;

    return ESP_OK;
}

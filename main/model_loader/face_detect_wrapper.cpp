/* ============================================================
 * face_detect_wrapper.cpp — HumanFaceDetect wrapper
 *
 * 照搬 esp_brookesia_phone 的人脸检测，SD 卡加载模型。
 * 模型: human_face_detect_msr_s8_v1.espdl + mnp_s8_v1.espdl
 * ============================================================ */

#include "face_detect_wrapper.hpp"
#include "human_face_detect.hpp"

#include "esp_log.h"
#include <cstdio>
#include <algorithm>

static const char *TAG = "FACE";

#define MODEL_DIR "/sdcard/models"

static HumanFaceDetect *s_detector = nullptr;

void face_detect_init(void)
{
    ESP_LOGI(TAG, "Loading face detect models from %s", MODEL_DIR);
    s_detector = new HumanFaceDetect(
        MODEL_DIR,
        HumanFaceDetect::model_type_t::MSRMNP_S8_V1);
    ESP_LOGI(TAG, "Face detector ready");
}

void face_detect_run(const uint16_t *rgb565, int width, int height,
                     face_detect_results_t *results)
{
    if (!s_detector || !rgb565 || !results) return;

    results->count = 0;

    dl::image::img_t img;
    img.data = (void *)rgb565;
    img.width = width;
    img.height = height;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565;

    auto &detect_results = s_detector->run(img);

    for (auto &res : detect_results) {
        if (results->count >= FACE_DETECT_MAX_FACES) break;
        auto &face = results->faces[results->count];
        face.x = res.box[0];
        face.y = res.box[1];
        face.w = res.box[2] - res.box[0];
        face.h = res.box[3] - res.box[1];
        face.confidence = res.score;
        results->count++;
    }
}

/* ---- bbox drawing (from esp_brookesia_phone) ---- */
void draw_rect_rgb565(uint16_t *buf, int width, int height,
                      int x, int y, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b, int thickness)
{
    int x1 = x, y1 = y, x2 = x + w, y2 = y + h;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= width) x2 = width - 1;
    if (y2 >= height) y2 = height - 1;

    uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);

    for (int t = 0; t < thickness; ++t) {
        for (int px = x1; px <= x2; ++px) {
            if (y1 + t >= 0 && y1 + t < height)
                buf[(y1 + t) * width + px] = color;
            if (y2 - t >= 0 && y2 - t < height)
                buf[(y2 - t) * width + px] = color;
        }
        for (int py = y1; py <= y2; ++py) {
            if (x1 + t >= 0 && x1 + t < width)
                buf[py * width + x1 + t] = color;
            if (x2 - t >= 0 && x2 - t < width)
                buf[py * width + x2 - t] = color;
        }
    }
}

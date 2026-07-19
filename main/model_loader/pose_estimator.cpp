/* ============================================================
 * pose_estimator.cpp — YOLO11n-pose 模型加载与推理
 *
 * 本文件是 C++ 源文件 (后缀 .cpp)，可以安全地 include
 * ESP-DL 的 C++ 头文件 (<limits>, <vector>, <map> 等)。
 *
 * 通过 pose_estimator.hpp 的 extern "C" 桥接暴露给 main.c 调用。
 * ============================================================ */

#include "pose_estimator.hpp"

/* ---------- ESP-DL 头文件 (C++ only) ---------- */
#include "dl_model_base.hpp"                 // dl::Model
#include "fbs_model.hpp"                     // fbs::MODEL_LOCATION_IN_SDCARD
#include "dl_tensor_base.hpp"                // TensorBase
#include "dl_image_preprocessor.hpp"         // ImagePreprocessor + img_t
#include "dl_image_define.hpp"               // DL_IMAGE_PIX_TYPE_xxx
#include "dl_pose_yolo11_postprocessor.hpp"  // yolo11posePostProcessor
#include "dl_detect_define.hpp"              // anchor_point_stage_t, result_t

#include "esp_log.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <dirent.h>

static const char *TAG = "POSE";

#define MODEL_PATH "/sdcard/models/yolo26n-pose_esp32p4.espdl"

/* ---------- 全局指针 ---------- */
static dl::Model                        *s_model     = nullptr;
static dl::image::ImagePreprocessor     *s_preproc   = nullptr;
static dl::detect::yolo11posePostProcessor *s_postproc = nullptr;

/* ============================================================
 * YOLO 预处理参数
 *
 * ImageNet 统计值 (均值和标准差)，与 Ultralytics YOLO11 训练时一致。
 * enable_letterbox() 使图像在缩放至 640×640 时保持宽高比，
 * 缺失部分用 114 灰度填充（YOLO 训练时的默认 padding 值）。
 * ============================================================ */
static constexpr float YOLO_MEAN[3] = {0.0f, 0.0f, 0.0f};
static constexpr float YOLO_STD[3]  = {255.0f, 255.0f, 255.0f};
static constexpr uint8_t YOLO_LETTERBOX_BG = 114;

/* YOLO11n-pose 的 3 个检测尺度 (P3/P4/P5) */
static const dl::detect::anchor_point_stage_t YOLO_STAGES[3] = {
    { 8,  8, 0, 0},   // P3/8  (80×80 网格)
    {16, 16, 0, 0},   // P4/16 (40×40 网格)
    {32, 32, 0, 0},   // P5/32 (20×20 网格)
};

static constexpr float SCORE_THR = 0.25f;  // 检测置信度阈值
static constexpr float NMS_THR   = 0.45f;  // NMS IoU 阈值
static constexpr int   TOP_K     = 5;       // 最多保留检测框数

/* ============================================================
 * 模型加载
 * ============================================================ */
void pose_estimator_load_and_print(void)
{
    ESP_LOGI(TAG, "Loading model from: %s", MODEL_PATH);

    /* ---- 调试：列出 SD 卡根目录的内容 ---- */
    DIR *root = opendir("/sdcard");
    if (root) {
        struct dirent *e;
        while ((e = readdir(root)) != NULL) {
            ESP_LOGI(TAG, "  %s%s", e->d_name, (e->d_type == DT_DIR) ? "/" : "");
        }
        closedir(root);
    } else {
        ESP_LOGE(TAG, "Cannot open /sdcard");
    }

    /* -------------------------------------------------------
     * 构造模型
     *   构造函数内部: 打开 .espdl → 解析 FlatBuffers 图结构
     *   → 创建所有 Module → 加载权重 → build() 分配内存
     * ------------------------------------------------------- */
    s_model = new (std::nothrow) dl::Model(
        MODEL_PATH,
        fbs::MODEL_LOCATION_IN_SDCARD,
        0,
        dl::MEMORY_MANAGER_GREEDY,
        nullptr,
        true
    );

    if (!s_model || !s_model->get_fbs_model()) {
        ESP_LOGE(TAG, "Model load FAILED — file not found or corrupted");
        delete s_model;
        s_model = nullptr;
        return;
    }

    ESP_LOGI(TAG, "Model loaded, printing info:");
    s_model->print();

    /* -------------------------------------------------------
     * 创建 ImagePreprocessor
     *   负责: 格式转换 → resize(640×640, letterbox) → 量化 int8
     * ------------------------------------------------------- */
    s_preproc = new (std::nothrow) dl::image::ImagePreprocessor(
        s_model,
        {YOLO_MEAN[0], YOLO_MEAN[1], YOLO_MEAN[2]},
        {YOLO_STD[0],  YOLO_STD[1],  YOLO_STD[2]},
        false                            // rgb_swap=false (输入已是 RGB)
    );
    if (!s_preproc) {
        ESP_LOGE(TAG, "Failed to create ImagePreprocessor");
        return;
    }
    s_preproc->enable_letterbox({YOLO_LETTERBOX_BG, YOLO_LETTERBOX_BG, YOLO_LETTERBOX_BG});

    /* -------------------------------------------------------
     * 创建 yolo11posePostProcessor
     *   负责: 解析 3 尺度输出 → NMS → 坐标逆映射回原图
     * ------------------------------------------------------- */
    s_postproc = new (std::nothrow) dl::detect::yolo11posePostProcessor(
        s_model,
        s_preproc,
        SCORE_THR, NMS_THR, TOP_K,
        std::vector<dl::detect::anchor_point_stage_t>(YOLO_STAGES, YOLO_STAGES + 3),
        1  // reg_max=1 (YOLO26 使用直接距离值，无需 DFL 解码)
    );
    if (!s_postproc) {
        ESP_LOGE(TAG, "Failed to create yolo11posePostProcessor");
        return;
    }

    ESP_LOGI(TAG, "Model + preprocessor + postprocessor ready");
}





/* ============================================================
 * 推理入口 — 被 app_main.c 的推理任务调用
 *
 * 输入: RGB565 帧 (来自相机回调的 PSRAM 缓冲)
 * 流程: preprocess → model->run() → postprocess → 坐标映射
 * 输出: joints[17][2] 归一化坐标 [-1, 1]
 * ============================================================ */
esp_err_t pose_estimator_run(const uint8_t *rgb565_buf,
                              uint32_t w, uint32_t h, uint32_t stride,
                              float joints_out[17][2],
                              float confidences[17],
                              float *score_out)
{
    /* ---- 检查模型是否已加载 ---- */
    if (!s_model || !s_preproc || !s_postproc) {
        ESP_LOGE(TAG, "Model not loaded, call pose_estimator_load_and_print() first");
        return ESP_ERR_INVALID_STATE;
    }
    if (!rgb565_buf || w == 0 || h == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- Step 1: 包装输入帧为 img_t ---- */
    dl::image::img_t src_img = {
        .data     = const_cast<uint8_t *>(rgb565_buf),
        .width    = static_cast<uint16_t>(w),
        .height   = static_cast<uint16_t>(h),
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
    };
    /* stride 由调用者保证 ≥ w * 2, ImagePreprocessor 内部使用
     * 传入的 img_t.width/height, 不依赖 stride */

    /* ---- Step 2: 预处理 (格式转换 + letterbox resize + 量化) ---- */
    s_preproc->preprocess(src_img);

    /* ---- Step 3: 模型推理 ---- */
    s_model->run();

    /* ---- Step 4: 后处理 (解析输出 + NMS) ---- */
    s_postproc->clear_result();
    s_postproc->postprocess();

    /* get_result(w, h) 将关键点坐标从 640×640 逆映射回原图尺寸 */
    std::list<dl::detect::result_t> &results = s_postproc->get_result(
        static_cast<int>(w), static_cast<int>(h));

    if (results.empty()) {
        /* 未检测到人体 */
        return ESP_FAIL;
    }

    /* ---- Step 5: 提取第一个人体 (置信度最高) ---- */
    const dl::detect::result_t &person = results.front();
    const std::vector<int> &kpt = person.keypoint;  // [x1,y1, x2,y2, ..., x17,y17]

    /* ---- Step 6: 像素坐标 → 归一化坐标 [-1, 1] ----
     *
     * 映射公式:
     *   norm_x = (x / w) * 2 - 1
     *   norm_y = (y / h) * 2 - 1   (注意 y 向上为负，与 UI 约定一致)
     *
     * 当 kpt[2*i] == 0 && kpt[2*i+1] == 0 时，表示该点
     * 置信度低于阈值，postprocessor 已将其置零。
     * ------------------------------------------------- */
    for (int i = 0; i < 17; i++) {
        int px = kpt[2 * i];
        int py = kpt[2 * i + 1];

        if (px == 0 && py == 0) {
            joints_out[i][0] = 0.0f;
            joints_out[i][1] = 0.0f;
            confidences[i]   = 0.0f;
        } else {
            joints_out[i][0] = (static_cast<float>(px) / static_cast<float>(w)) * 2.0f - 1.0f;
            joints_out[i][1] = (static_cast<float>(py) / static_cast<float>(h)) * 2.0f - 1.0f;
            confidences[i]   = 1.0f;  // 简化: 有坐标即高置信度
        }
    }

    if (score_out) {
        *score_out = person.score;
    }

    return ESP_OK;
}

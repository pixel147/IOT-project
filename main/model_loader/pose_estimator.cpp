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
#include "dl_model_base.hpp"              // dl::Model
#include "fbs_model.hpp"                  // fbs::MODEL_LOCATION_IN_SDCARD
#include "dl_tensor_base.hpp"             // TensorBase
#include "dl_image_preprocessor.hpp"      // ImagePreprocessor
#include "dl_image_define.hpp"            // img_t

#include "esp_log.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <dirent.h>

static const char *TAG = "POSE";

#define MODEL_PATH "/sdcard/models/coco_pose_yolo11n_pose_s8_v2.espdl"

/* ---------- 全局模型指针 ---------- */
static dl::Model *s_model = nullptr;

void pose_estimator_load_and_print(void)
{
    ESP_LOGI(TAG, "Loading model from: %s", MODEL_PATH);

    /* ---- 调试：列出 SD 卡根目录的内容 ---- */
    ESP_LOGI(TAG, "--- Listing /sdcard/ ---");
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
    ESP_LOGI(TAG, "--- end of listing ---");

    /* -------------------------------------------------------
     * dl::Model 构造:
     *   参数1: 模型路径
     *   参数2: MODEL_LOCATION_IN_SDCARD — 从 SD 卡读取
     *   参数3: max_internal_size = 0 (自动分配)
     *   参数4: MEMORY_MANAGER_GREEDY   — 内存复用策略
     *   参数5: key = nullptr           — 不加密
     *   参数6: param_copy = true       — 参数拷到 PSRAM 加速
     *
     *   构造函数内部自动: 打开文件 → 解析 FlatBuffers
     *   → 创建 Module 列表 → 加载权重 → build() 分配内存
     * ------------------------------------------------------- */
    s_model = new (std::nothrow) dl::Model(
        MODEL_PATH,
        fbs::MODEL_LOCATION_IN_SDCARD,
        0,
        dl::MEMORY_MANAGER_GREEDY,
        nullptr,
        true
    );

    /* 注意: dl::Model 构造函数失败时不一定返回 nullptr,
     * 需要检查 get_fbs_model() 来判断加载是否成功 */
    if (!s_model || !s_model->get_fbs_model()) {
        ESP_LOGE(TAG, "Model load FAILED — file not found or corrupted");
        ESP_LOGE(TAG, "Check: (1) SD card mounted? (2) Path = %s", MODEL_PATH);
        ESP_LOGE(TAG, "Check: (3) File is valid .espdl for ESP32-P4?");
        delete s_model;
        s_model = nullptr;
        return;
    }

    ESP_LOGI(TAG, "Model loaded, printing info:");
    s_model->print();

    // /* 打印输入 tensor */
    // auto &inputs = s_model->get_inputs();
    // for (auto &[name, tensor] : inputs) {
    //     printf("  Input: %s, shape=[", name.c_str());
    //     for (int d : tensor->shape) printf("%d ", d);
    //     printf("]\n");
    // }

    // /* 打印输出 tensor */
    // auto &outputs = s_model->get_outputs();
    // for (auto &[name, tensor] : outputs) {
    //     printf("  Output: %s, shape=[", name.c_str());
    //     for (int d : tensor->shape) printf("%d ", d);
    //     printf("]\n");
    // }

    ESP_LOGI(TAG, "Model loaded successfully");
}

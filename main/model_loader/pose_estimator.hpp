#ifndef POSE_ESTIMATOR_HPP
#define POSE_ESTIMATOR_HPP

#include "esp_err.h"

/* ============================================================
 * pose_estimator.hpp — C/C++ 桥接头文件
 *
 * 这个头文件可以被 C 和 C++ 同时包含:
 * - C 编译器看到 extern "C" 之外的纯 C 声明
 * - C++ 编译器看到 extern "C" 块，链接时用 C 符号表
 *
 * 所有 C++ 代码 (ESP-DL) 都在 pose_estimator.cpp 里，
 * main.c 作为纯 C 文件，只通过这里暴露的函数与 C++ 交互。
 * ============================================================ */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 加载 YOLO11n-pose .espdl 模型并打印信息
 *        在 app_main() 中调用，SD 卡必须已挂载
 */
void pose_estimator_load_and_print(void);

/**
 * @brief 对一帧 RGB565 图像运行姿态推理
 *
 * @param[in]  rgb565_buf    RGB565 图像数据
 * @param[in]  w             图像宽度（像素）
 * @param[in]  h             图像高度（像素）
 * @param[in]  stride        每行字节数
 * @param[out] joints_out    COCO 17 关键点，归一化坐标 [-1, 1]
 * @param[out] confidences   各关键点置信度 [0~1]
 * @param[out] score_out     整体检测分数
 * @return ESP_OK  成功检测到人体
 * @return ESP_FAIL 未检测到人体或模型未加载
 */
esp_err_t pose_estimator_run(const uint8_t *rgb565_buf,
                              uint32_t w, uint32_t h, uint32_t stride,
                              float joints_out[17][2],
                              float confidences[17],
                              float *score_out);

#ifdef __cplusplus
}
#endif

#endif /* POSE_ESTIMATOR_HPP */

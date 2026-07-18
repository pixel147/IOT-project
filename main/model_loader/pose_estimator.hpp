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

#ifdef __cplusplus
}
#endif

#endif /* POSE_ESTIMATOR_HPP */

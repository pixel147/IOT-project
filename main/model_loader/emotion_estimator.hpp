#ifndef EMOTION_ESTIMATOR_HPP
#define EMOTION_ESTIMATOR_HPP

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 加载 EmotionCNN .espdl 模型（SD 卡必须已挂载）
 */
void emotion_estimator_load(void);

/**
 * @brief 对 RGB565 帧中的面部区域运行情绪识别
 *
 * @param[in]  rgb565_buf     RGB565 帧数据（完整帧）
 * @param[in]  frame_w        帧宽度（像素）
 * @param[in]  frame_h        帧高度（像素）
 * @param[in]  frame_stride   每行字节数
 * @param[in]  face_x         面部 ROI 左上角 x
 * @param[in]  face_y         面部 ROI 左上角 y
 * @param[in]  face_w         面部 ROI 宽度
 * @param[in]  face_h         面部 ROI 高度
 * @param[out] emotion_class  情绪类别 0-6
 *                            0=Angry 1=Disgust 2=Fear 3=Happy
 *                            4=Sad 5=Surprise 6=Neutral
 * @param[out] confidence     置信度 [0, 1]
 * @return ESP_OK 成功 / ESP_FAIL 推理失败
 */
esp_err_t emotion_estimator_run(const uint8_t *rgb565_buf,
                                uint32_t frame_w, uint32_t frame_h,
                                uint32_t frame_stride,
                                int face_x, int face_y,
                                int face_w, int face_h,
                                int *emotion_class,
                                float *confidence);

#ifdef __cplusplus
}
#endif

#endif /* EMOTION_ESTIMATOR_HPP */

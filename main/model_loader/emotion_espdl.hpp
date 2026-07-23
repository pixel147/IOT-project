#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load ESP-DL emotion model from SD card.
 *        SD card must be mounted at /sdcard.
 */
void emotion_espdl_load(void);

/**
 * @brief Run emotion recognition on a face crop via ESP-DL.
 * @param rgb565     Full frame RGB565 buffer
 * @param frame_w    Frame width
 * @param frame_h    Frame height
 * @param stride     Bytes per row (w*2 for RGB565)
 * @param face_x,y,w,h Face ROI in pixel coords
 * @param[out] emotion_class  0=Angry..6=Neutral
 * @param[out] confidence     [0.0, 1.0]
 */
esp_err_t emotion_espdl_run(const uint8_t *rgb565,
                             int frame_w, int frame_h, int stride,
                             int face_x, int face_y, int face_w, int face_h,
                             int *emotion_class, float *confidence);

#ifdef __cplusplus
}
#endif

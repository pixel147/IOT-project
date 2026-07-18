#ifndef CAM_H
#define CAM_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

typedef void (*cam_frame_cb_t)(const uint8_t *buf, uint32_t len,
                                 uint32_t w, uint32_t h, uint32_t stride,
                                 uint32_t fmt);

esp_err_t cam_start(uint32_t width, uint32_t height, uint32_t fps, cam_frame_cb_t cb);
void cam_stop(void);
bool cam_is_running(void);
float cam_get_fps(void);
const uint8_t *cam_get_frame_buffer(uint32_t *len);
void cam_get_frame_info(uint32_t *w, uint32_t *h);

#endif

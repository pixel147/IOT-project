#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(void);

lv_obj_t *ui_get_camera_container(void);

void ui_update_camera_preview(const uint8_t *buf, uint32_t w, uint32_t h,
                              uint32_t stride);
void ui_get_preview_rect(int *x, int *y, int *w, int *h);
void ui_update_fps(float fps);
void ui_set_system_status(const char *status);
void ui_set_mode_text(const char *mode);
void ui_update_suggestion(const char *text);
void ui_show_message(const char *title, const char *msg, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif

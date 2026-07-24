#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建监控屏幕（独立 lv_screen），在 app_main 中调用一次 */
lv_obj_t *ui_create_monitor_screen(void);

/** 设置返回按钮回调（→ 回到 Home） */
void ui_monitor_set_back_callback(lv_event_cb_t cb);

lv_obj_t *ui_get_camera_container(void);

void ui_update_camera_preview(const uint8_t *buf, uint32_t w, uint32_t h,
                              uint32_t stride);
void ui_update_fps(float fps);
void ui_set_system_status(const char *status);
void ui_set_mode_text(const char *mode);
void ui_update_suggestion(const char *text);

void ui_update_emotion(int emotion_class, float confidence);
void ui_hide_emotion(void);
bool ui_is_monitoring(void);

void ui_show_message(const char *title, const char *msg, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif

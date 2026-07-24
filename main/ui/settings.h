/* ============================================================
 * settings.h — 设置页面（WiFi 扫描 + 显示）
 * ============================================================ */

#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建设置屏幕（含 WiFi 扫描），返回 screen 对象 */
lv_obj_t *ui_settings_create(void);

/** 设置返回按钮回调 */
void ui_settings_set_back_callback(lv_event_cb_t cb);

/** 触发 WiFi 扫描（由 app_main 在 WiFi 就绪后调用） */
void ui_settings_scan_wifi(void);

#ifdef __cplusplus
}
#endif

#endif

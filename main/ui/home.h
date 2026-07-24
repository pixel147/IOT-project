/* ============================================================
 * home.h — 主菜单界面（App 桌面）
 *
 * 仿 ESP_Brookesia Phone 风格，图标网格启动各个功能
 * ============================================================ */

#ifndef UI_HOME_H
#define UI_HOME_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建 Home 屏幕，返回 screen 对象 */
lv_obj_t *ui_home_create(void);

/** 设置图标点击回调
 * @param icon_index  0=情绪监控, 1=AI聊天, 2=设置
 * @param cb          回调函数
 */
void ui_home_set_icon_callback(int icon_index, lv_event_cb_t cb);

/** 获取 Home 屏幕对象 */
lv_obj_t *ui_home_get_screen(void);

#ifdef __cplusplus
}
#endif

#endif

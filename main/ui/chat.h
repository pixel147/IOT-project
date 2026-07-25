#ifndef UI_CHAT_H
#define UI_CHAT_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_chat_create(void);
void ui_chat_set_back_callback(lv_event_cb_t cb);

/** 从摄像头回调更新聊天页情绪徽章（实时反映检测到的情绪） */
void ui_chat_update_emotion(int cls, float conf);

/* These entry points are for the XiaoZhi voice task. They take the LVGL lock
 * themselves, so they are safe to call from an audio or network callback. */
void ui_chat_voice_append_user(const char *text);
void ui_chat_voice_append_assistant(const char *text);
void ui_chat_voice_set_status(const char *status);
typedef void (*ui_chat_voice_start_cb_t)(void);
void ui_chat_set_voice_start_callback(ui_chat_voice_start_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* UI_CHAT_H */

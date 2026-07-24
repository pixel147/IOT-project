#ifndef UI_CHAT_H
#define UI_CHAT_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_chat_create(void);
void ui_chat_set_back_callback(lv_event_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* UI_CHAT_H */

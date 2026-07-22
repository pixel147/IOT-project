#include "ui.h"
#include "ui_font_zh_22.h"

#include <string.h>

LV_FONT_DECLARE(lv_font_montserrat_14);

static lv_obj_t *camera_container;
static lv_obj_t *camera_img;
static lv_obj_t *fps_label;
static lv_obj_t *status_label;
static lv_obj_t *mode_label_statusbar;
static lv_obj_t *suggestion_panel;
static lv_obj_t *suggestion_label;
static lv_obj_t *message_popup;
static lv_timer_t *message_timer;

static uint32_t preview_width = 4;
static uint32_t preview_height = 3;

static void set_ui_text_font(lv_obj_t *obj)
{
    lv_obj_set_style_text_font(obj, &ui_font_zh_22, 0);
}

static void set_symbol_font(lv_obj_t *obj)
{
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
}

static void set_panel_style(lv_obj_t *obj, lv_color_t color)
{
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
}

static void hide_message_cb(lv_timer_t *timer)
{
    if (message_popup) {
        lv_obj_add_flag(message_popup, LV_OBJ_FLAG_HIDDEN);
    }
    message_timer = NULL;
    lv_timer_del(timer);
}

lv_obj_t *ui_get_camera_container(void)
{
    return camera_container;
}

void ui_create_main_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0E27), 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* ---- 状态栏 ---- */
    lv_obj_t *status_bar = lv_obj_create(scr);
    lv_obj_set_size(status_bar, LV_PCT(100), 46);
    set_panel_style(status_bar, lv_color_hex(0x18213D));
    lv_obj_set_style_pad_hor(status_bar, 16, 0);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *time_label = lv_label_create(status_bar);
    lv_label_set_text(time_label, "12:36");
    set_symbol_font(time_label);
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);

    mode_label_statusbar = lv_label_create(status_bar);
    lv_label_set_text(mode_label_statusbar, "情绪聊天");
    set_ui_text_font(mode_label_statusbar);
    lv_obj_set_style_text_color(mode_label_statusbar, lv_color_hex(0x4FC3F7), 0);

    status_label = lv_label_create(status_bar);
    lv_label_set_text(status_label, "启动中…");
    set_ui_text_font(status_label);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x8B949E), 0);

    fps_label = lv_label_create(status_bar);
    lv_label_set_text(fps_label, "0 FPS");
    set_symbol_font(fps_label);
    lv_obj_set_style_text_color(fps_label, lv_color_hex(0x8B949E), 0);

    lv_obj_t *icons = lv_label_create(status_bar);
    lv_label_set_text(icons, LV_SYMBOL_WIFI "  " LV_SYMBOL_BATTERY_FULL);
    set_symbol_font(icons);
    lv_obj_set_style_text_color(icons, lv_color_white(), 0);

    /* ---- 中间区域：相机预览（全宽） ---- */
    lv_obj_t *middle_row = lv_obj_create(scr);
    lv_obj_set_size(middle_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(middle_row, 1);
    lv_obj_set_flex_flow(middle_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(middle_row, 0, 0);
    lv_obj_set_style_border_width(middle_row, 0, 0);
    lv_obj_set_style_bg_opa(middle_row, LV_OPA_TRANSP, 0);

    lv_obj_t *camera_stage = lv_obj_create(middle_row);
    lv_obj_set_flex_grow(camera_stage, 1);
    lv_obj_set_height(camera_stage, LV_PCT(100));
    set_panel_style(camera_stage, lv_color_hex(0x101428));
    lv_obj_set_style_pad_all(camera_stage, 0, 0);
    lv_obj_set_scrollbar_mode(camera_stage, LV_SCROLLBAR_MODE_OFF);

    camera_container = lv_obj_create(camera_stage);
    lv_obj_set_size(camera_container, LV_PCT(100), LV_PCT(100));
    lv_obj_center(camera_container);
    lv_obj_set_style_bg_color(camera_container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(camera_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(camera_container, 0, 0);
    lv_obj_set_style_pad_all(camera_container, 0, 0);
    lv_obj_set_scrollbar_mode(camera_container, LV_SCROLLBAR_MODE_OFF);

    camera_img = lv_canvas_create(camera_container);
    lv_obj_set_size(camera_img, LV_PCT(100), LV_PCT(100));
    lv_obj_center(camera_img);

    /* ---- 建议栏 ---- */
    suggestion_panel = lv_obj_create(scr);
    lv_obj_set_size(suggestion_panel, LV_PCT(100), LV_SIZE_CONTENT);
    set_panel_style(suggestion_panel, lv_color_hex(0x101428));
    lv_obj_set_style_pad_all(suggestion_panel, 12, 0);
    lv_obj_set_style_border_side(suggestion_panel, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(suggestion_panel, 1, 0);
    lv_obj_set_style_border_color(suggestion_panel, lv_color_hex(0x30363D), 0);
    suggestion_label = lv_label_create(suggestion_panel);
    lv_obj_set_width(suggestion_label, LV_PCT(100));
    lv_label_set_long_mode(suggestion_label, LV_LABEL_LONG_WRAP);
    set_ui_text_font(suggestion_label);
    lv_obj_set_style_text_color(suggestion_label, lv_color_hex(0xFFC107), 0);
    lv_label_set_text(suggestion_label, "系统已启动，等待功能接入…");

    /* ---- 控制栏 ---- */
    lv_obj_t *control_bar = lv_obj_create(scr);
    lv_obj_set_size(control_bar, LV_PCT(100), 56);
    set_panel_style(control_bar, lv_color_hex(0x18213D));
    lv_obj_set_flex_flow(control_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(control_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(control_bar, 18, 0);

    lv_obj_t *settings_btn = lv_btn_create(control_bar);
    lv_obj_set_size(settings_btn, 56, 40);
    lv_obj_t *settings_icon = lv_label_create(settings_btn);
    lv_label_set_text(settings_icon, LV_SYMBOL_SETTINGS);
    set_symbol_font(settings_icon);
    lv_obj_center(settings_icon);

    /* ---- 消息弹窗 ---- */
    message_popup = lv_obj_create(scr);
    lv_obj_set_size(message_popup, LV_PCT(58), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(message_popup, lv_color_hex(0x21262D), 0);
    lv_obj_set_style_border_width(message_popup, 1, 0);
    lv_obj_set_style_border_color(message_popup, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_radius(message_popup, 6, 0);
    lv_obj_set_style_pad_all(message_popup, 16, 0);
    lv_obj_set_flex_flow(message_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(message_popup, 8, 0);
    lv_obj_center(message_popup);
    lv_obj_add_flag(message_popup, LV_OBJ_FLAG_HIDDEN);
}

/* ---- 公共 API ---- */

void ui_update_camera_preview(const uint8_t *buf, uint32_t w, uint32_t h,
                              uint32_t stride)
{
    if (!camera_img || !buf || w == 0 || h == 0 || stride < w * 2) return;

    preview_width = w;
    preview_height = h;
    lv_canvas_set_buffer(camera_img, (void *)buf, (lv_coord_t)w, (lv_coord_t)h,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(camera_img);
}

void ui_update_fps(float fps)
{
    if (fps_label) {
        lv_label_set_text_fmt(fps_label, "%.0f FPS", (double)fps);
    }
}

void ui_get_preview_rect(int *x, int *y, int *w, int *h)
{
    if (!camera_img) {
        *x = 0; *y = 0; *w = 320; *h = 240;
        return;
    }
    *x = lv_obj_get_x(camera_img);
    *y = lv_obj_get_y(camera_img);
    *w = lv_obj_get_width(camera_img);
    *h = lv_obj_get_height(camera_img);
}

void ui_set_system_status(const char *status)
{
    if (status_label && status) {
        lv_label_set_text(status_label, status);
    }
}

void ui_set_mode_text(const char *mode)
{
    if (mode_label_statusbar && mode) {
        lv_label_set_text(mode_label_statusbar, mode);
    }
}

void ui_update_suggestion(const char *text)
{
    if (!suggestion_panel || !suggestion_label) return;
    if (!text || text[0] == '\0') {
        lv_label_set_text(suggestion_label, "");
        return;
    }
    lv_label_set_text(suggestion_label, text);
    lv_obj_clear_flag(suggestion_panel, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_message(const char *title, const char *msg, uint32_t duration_ms)
{
    if (!message_popup) return;
    if (message_timer) {
        lv_timer_del(message_timer);
        message_timer = NULL;
    }

    lv_obj_clean(message_popup);
    lv_obj_t *title_label = lv_label_create(message_popup);
    lv_label_set_text(title_label, title ? title : "");
    set_ui_text_font(title_label);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);

    lv_obj_t *message_label = lv_label_create(message_popup);
    lv_label_set_text(message_label, msg ? msg : "");
    set_ui_text_font(message_label);
    lv_obj_set_style_text_color(message_label, lv_color_hex(0xC9D1D9), 0);
    lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(message_label, LV_PCT(100));
    lv_obj_clear_flag(message_popup, LV_OBJ_FLAG_HIDDEN);

    if (duration_ms > 0) {
        message_timer = lv_timer_create(hide_message_cb, duration_ms, NULL);
    }
}

void ui_init(void)
{
    ui_create_main_screen();
}

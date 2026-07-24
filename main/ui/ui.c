/* ============================================================
 * ui.c — 情绪守护 UI (深色主题)
 * 布局: 状态栏 | 左图例 + 相机预览 + 右信息 | 建议栏 | 控制栏
 * ============================================================ */

#include "ui.h"
#include "ui_font_zh_22.h"
#include <string.h>
#include <stdio.h>

LV_FONT_DECLARE(lv_font_montserrat_14);

static lv_obj_t *camera_container;
static lv_obj_t *camera_img;
static lv_obj_t *fps_label;
static lv_obj_t *status_label;
static lv_obj_t *mode_label;
static lv_obj_t *suggestion_label;
static lv_obj_t *message_popup;
static lv_timer_t *message_timer;
static lv_obj_t *emotion_badge;
static lv_obj_t *badge_name;
static lv_obj_t *badge_conf;
static lv_obj_t *right_panel;
static lv_obj_t *right_name;
static lv_obj_t *right_conf;
static lv_obj_t *right_tip;
static lv_obj_t *right_time;
static lv_obj_t *monitor_btn;
static lv_obj_t *s_monitor_screen;
static lv_obj_t *s_back_btn;
static bool s_monitoring = true;

/* 情绪数据 */
static const char *CN[7] = {"生气", "厌恶", "害怕", "开心", "难过", "惊讶", "平静"};
static const uint32_t CLR[7] = {0xE53935, 0x2E7D32, 0x1565C0, 0x43A047, 0xE53935, 0xFDD835, 0x78909C};
static const char *TIP[7] = {
    "深呼吸冷静一下", "试着放宽心吧", "别怕你很安全",
    "保持好心情",     "想点开心的事", "哇真惊喜呢",
    "享受此刻宁静"
};

static void font_zh(lv_obj_t *o) { lv_obj_set_style_text_font(o, &ui_font_zh_22, 0); }
static void font_en(lv_obj_t *o) { lv_obj_set_style_text_font(o, &lv_font_montserrat_14, 0); }

static void pnl_style(lv_obj_t *o, lv_color_t c)
{
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
}

static void hide_msg(lv_timer_t *t)
{
    if (message_popup) lv_obj_add_flag(message_popup, LV_OBJ_FLAG_HIDDEN);
    message_timer = NULL; lv_timer_del(t);
}

static void btn_fx(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_target(e);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED)
        lv_obj_set_style_bg_opa(b, LV_OPA_30, 0);
    else if (lv_event_get_code(e) == LV_EVENT_RELEASED || lv_event_get_code(e) == LV_EVENT_PRESS_LOST)
        lv_obj_set_style_bg_opa(b, LV_OPA_0, 0);
}

static lv_obj_t *mk_btn(lv_obj_t *p, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x484F58), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, btn_fx, LV_EVENT_ALL, NULL);
    return b;
}

static void toggle_monitor(lv_event_t *e)
{
    (void)e; s_monitoring = !s_monitoring;
    lv_obj_t *l = lv_obj_get_child(monitor_btn, 0);
    if (s_monitoring) {
        lv_label_set_text(l, "监控中");
        lv_obj_set_style_bg_color(monitor_btn, lv_color_hex(0x2EA043), 0);
        ui_update_suggestion("情绪监控已开启");
    } else {
        lv_label_set_text(l, "已暂停");
        lv_obj_set_style_bg_color(monitor_btn, lv_color_hex(0xDA3633), 0);
        ui_update_suggestion("情绪监控已暂停");
    }
}

bool ui_is_monitoring(void) { return s_monitoring; }
lv_obj_t *ui_get_camera_container(void) { return camera_container; }

void ui_create_main_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    s_monitor_screen = scr;
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);

    /* ========== 导航栏 ========== */
    lv_obj_t *nav = lv_obj_create(scr);
    lv_obj_set_size(nav, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(nav, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_width(nav, 0, 0);
    lv_obj_set_style_radius(nav, 0, 0);
    lv_obj_set_style_pad_hor(nav, 8, 0);
    lv_obj_set_style_pad_ver(nav, 0, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 返回按钮 */
    s_back_btn = lv_btn_create(nav);
    lv_obj_set_size(s_back_btn, 36, 36);
    lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(s_back_btn, 0, 0);
    lv_obj_set_style_radius(s_back_btn, 8, 0);
    lv_obj_set_style_shadow_width(s_back_btn, 0, 0);
    lv_obj_add_event_cb(s_back_btn, btn_fx, LV_EVENT_ALL, NULL);
    lv_obj_t *back_sym = lv_label_create(s_back_btn);
    lv_label_set_text(back_sym, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_sym, lv_color_hex(0xC9D1D9), 0);
    lv_obj_center(back_sym);

    /* 标题 */
    lv_obj_t *nav_title = lv_label_create(nav);
    lv_label_set_text(nav_title, "情绪守护");
    font_zh(nav_title);
    lv_obj_set_style_text_color(nav_title, lv_color_hex(0x58A6FF), 0);
    lv_obj_set_style_margin_left(nav_title, 12, 0);

    /* ========== 状态栏 ========== */
    lv_obj_t *sb = lv_obj_create(scr);
    lv_obj_set_size(sb, LV_PCT(100), 44);
    pnl_style(sb, lv_color_hex(0x161B22));
    lv_obj_set_style_pad_hor(sb, 14, 0);
    lv_obj_set_style_pad_top(sb, 0, 0);
    lv_obj_set_style_pad_bottom(sb, 0, 0);
    lv_obj_set_flex_flow(sb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sb, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mode_label = lv_label_create(sb);
    lv_label_set_text(mode_label, "情绪守护");
    font_zh(mode_label);
    lv_obj_set_style_text_color(mode_label, lv_color_hex(0x58A6FF), 0);

    status_label = lv_label_create(sb);
    lv_label_set_text(status_label, "启动中");
    font_zh(status_label);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x8B949E), 0);

    fps_label = lv_label_create(sb);
    lv_label_set_text(fps_label, "0 FPS");
    font_en(fps_label);
    lv_obj_set_style_text_color(fps_label, lv_color_hex(0x8B949E), 0);

    /* ========== 中间行：左图例 | 相机 | 右信息 ========== */
    lv_obj_t *mid = lv_obj_create(scr);
    lv_obj_set_size(mid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_style_pad_all(mid, 0, 0);
    lv_obj_set_style_border_width(mid, 0, 0);
    lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);

    /* ---- 左：竖排图例 ---- */
    lv_obj_t *lgd = lv_obj_create(mid);
    lv_obj_set_size(lgd, 72, LV_PCT(100));
    pnl_style(lgd, lv_color_hex(0x161B22));
    lv_obj_set_style_pad_all(lgd, 4, 0);
    lv_obj_set_style_pad_top(lgd, 8, 0);
    lv_obj_set_style_pad_bottom(lgd, 8, 0);
    lv_obj_set_flex_flow(lgd, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(lgd, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_side(lgd, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(lgd, 1, 0);
    lv_obj_set_style_border_color(lgd, lv_color_hex(0x21262D), 0);

    for (int i = 0; i < 7; i++) {
        lv_obj_t *it = lv_obj_create(lgd);
        lv_obj_set_size(it, 56, 30);
        lv_obj_set_style_bg_color(it, lv_color_hex(CLR[i]), 0);
        lv_obj_set_style_bg_opa(it, LV_OPA_20, 0);
        lv_obj_set_style_radius(it, 15, 0);
        lv_obj_set_style_border_width(it, 2, 0);
        lv_obj_set_style_border_color(it, lv_color_hex(CLR[i]), 0);
        lv_obj_set_style_pad_all(it, 0, 0);
        lv_obj_t *lb = lv_label_create(it);
        lv_label_set_text(lb, CN[i]);
        font_zh(lb);
        lv_obj_set_style_text_color(lb, lv_color_hex(CLR[i]), 0);
        lv_obj_center(lb);
    }

    /* ---- 相机 ---- */
    lv_obj_t *stage = lv_obj_create(mid);
    lv_obj_set_flex_grow(stage, 1);
    lv_obj_set_height(stage, LV_PCT(100));
    lv_obj_set_style_pad_all(stage, 0, 0);
    lv_obj_set_style_border_width(stage, 0, 0);
    lv_obj_set_style_bg_opa(stage, LV_OPA_TRANSP, 0);

    camera_container = lv_obj_create(stage);
    lv_obj_set_size(camera_container, LV_PCT(100), LV_PCT(100));
    lv_obj_center(camera_container);
    lv_obj_set_style_bg_color(camera_container, lv_color_hex(0x010409), 0);
    lv_obj_set_style_border_width(camera_container, 0, 0);
    lv_obj_set_style_pad_all(camera_container, 0, 0);
    lv_obj_set_scrollbar_mode(camera_container, LV_SCROLLBAR_MODE_OFF);

    camera_img = lv_canvas_create(camera_container);
    lv_obj_set_size(camera_img, LV_PCT(100), LV_PCT(100));
    lv_obj_center(camera_img);

    /* 徽章（相机左上角） */
    emotion_badge = lv_obj_create(camera_container);
    lv_obj_set_size(emotion_badge, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_bg_color(emotion_badge, lv_color_hex(CLR[6]), 0);
    lv_obj_set_style_bg_opa(emotion_badge, LV_OPA_80, 0);
    lv_obj_set_style_radius(emotion_badge, 15, 0);
    lv_obj_set_style_border_width(emotion_badge, 0, 0);
    lv_obj_set_style_pad_hor(emotion_badge, 10, 0);
    lv_obj_set_pos(emotion_badge, 8, 8);
    lv_obj_set_flex_flow(emotion_badge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(emotion_badge, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(emotion_badge, 4, 0);
    lv_obj_add_flag(emotion_badge, LV_OBJ_FLAG_HIDDEN);

    badge_name = lv_label_create(emotion_badge);
    lv_label_set_text(badge_name, "平静");
    font_zh(badge_name);
    lv_obj_set_style_text_color(badge_name, lv_color_white(), 0);

    badge_conf = lv_label_create(emotion_badge);
    lv_label_set_text(badge_conf, "");
    font_en(badge_conf);
    lv_obj_set_style_text_color(badge_conf, lv_color_white(), 0);
    lv_obj_set_style_text_opa(badge_conf, LV_OPA_70, 0);

    /* ---- 右：信息面板 ---- */
    right_panel = lv_obj_create(mid);
    lv_obj_set_size(right_panel, 120, LV_PCT(100));
    pnl_style(right_panel, lv_color_hex(0x161B22));
    lv_obj_set_style_pad_all(right_panel, 0, 0);     /* 由 flex 内部控制间距 */
    lv_obj_set_style_border_side(right_panel, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(right_panel, 1, 0);
    lv_obj_set_style_border_color(right_panel, lv_color_hex(0x21262D), 0);
    lv_obj_set_flex_flow(right_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 顶部弹簧占位 */
    lv_obj_t *spacer_top = lv_obj_create(right_panel);
    lv_obj_set_size(spacer_top, 1, LV_PCT(15));
    lv_obj_set_style_bg_opa(spacer_top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer_top, 0, 0);

    /* 情绪名称 */
    right_name = lv_label_create(right_panel);
    lv_label_set_text(right_name, "平静");
    font_zh(right_name);
    lv_obj_set_style_text_color(right_name, lv_color_hex(CLR[6]), 0);

    /* 置信度 */
    right_conf = lv_label_create(right_panel);
    lv_label_set_text(right_conf, "---");
    font_en(right_conf);
    lv_obj_set_style_text_color(right_conf, lv_color_hex(0x8B949E), 0);

    /* 分隔 */
    lv_obj_t *sep = lv_obj_create(right_panel);
    lv_obj_set_size(sep, 72, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);
    lv_obj_set_style_margin_top(sep, 8, 0);
    lv_obj_set_style_margin_bottom(sep, 8, 0);

    /* 小贴士 */
    right_tip = lv_label_create(right_panel);
    lv_label_set_text(right_tip, "享受此刻宁静");
    font_zh(right_tip);
    lv_obj_set_width(right_tip, LV_PCT(90));
    lv_label_set_long_mode(right_tip, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(right_tip, lv_color_hex(0xFFC107), 0);

    /* 时长 */
    right_time = lv_label_create(right_panel);
    lv_label_set_text(right_time, "0 min");
    font_en(right_time);
    lv_obj_set_style_text_color(right_time, lv_color_hex(0x484F58), 0);
    lv_obj_set_style_margin_top(right_time, 8, 0);

    /* 底部弹簧占位 */
    lv_obj_t *spacer_btm = lv_obj_create(right_panel);
    lv_obj_set_size(spacer_btm, 1, LV_PCT(20));
    lv_obj_set_flex_grow(spacer_btm, 1);
    lv_obj_set_style_bg_opa(spacer_btm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer_btm, 0, 0);

    /* ========== 建议栏 ========== */
    lv_obj_t *sg = lv_obj_create(scr);
    lv_obj_set_size(sg, LV_PCT(100), LV_SIZE_CONTENT);
    pnl_style(sg, lv_color_hex(0x0D1117));
    lv_obj_set_style_pad_hor(sg, 14, 0);
    lv_obj_set_style_pad_ver(sg, 8, 0);
    lv_obj_set_style_border_side(sg, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(sg, 1, 0);
    lv_obj_set_style_border_color(sg, lv_color_hex(0x21262D), 0);

    suggestion_label = lv_label_create(sg);
    lv_obj_set_width(suggestion_label, LV_PCT(100));
    lv_label_set_long_mode(suggestion_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    font_zh(suggestion_label);
    lv_obj_set_style_text_color(suggestion_label, lv_color_hex(0xFFC107), 0);
    lv_label_set_text(suggestion_label, "系统启动中");

    /* ========== 控制栏 ========== */
    lv_obj_t *ctrl = lv_obj_create(scr);
    lv_obj_set_size(ctrl, LV_PCT(100), 52);
    pnl_style(ctrl, lv_color_hex(0x161B22));
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl, 16, 0);
    lv_obj_set_style_border_side(ctrl, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(ctrl, 1, 0);
    lv_obj_set_style_border_color(ctrl, lv_color_hex(0x21262D), 0);

    monitor_btn = mk_btn(ctrl, 100, 38);
    lv_obj_set_style_bg_color(monitor_btn, lv_color_hex(0x2EA043), 0);
    lv_obj_add_event_cb(monitor_btn, toggle_monitor, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ml = lv_label_create(monitor_btn);
    lv_label_set_text(ml, "监控中");
    font_zh(ml);
    lv_obj_set_style_text_color(ml, lv_color_white(), 0);
    lv_obj_center(ml);

    lv_obj_t *set_btn = mk_btn(ctrl, 48, 38);
    lv_obj_t *si = lv_label_create(set_btn);
    lv_label_set_text(si, LV_SYMBOL_SETTINGS);
    font_en(si);
    lv_obj_center(si);
    lv_obj_set_style_text_color(si, lv_color_hex(0xC9D1D9), 0);

    /* ========== 消息弹窗 ========== */
    message_popup = lv_obj_create(scr);
    lv_obj_set_size(message_popup, LV_PCT(60), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(message_popup, lv_color_hex(0x21262D), 0);
    lv_obj_set_style_border_width(message_popup, 1, 0);
    lv_obj_set_style_border_color(message_popup, lv_color_hex(0x58A6FF), 0);
    lv_obj_set_style_radius(message_popup, 12, 0);
    lv_obj_set_style_pad_all(message_popup, 16, 0);
    lv_obj_set_flex_flow(message_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_center(message_popup);
    lv_obj_add_flag(message_popup, LV_OBJ_FLAG_HIDDEN);
}

/* ============================================================
 * 公开 API
 * ============================================================ */

void ui_update_camera_preview(const uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride)
{
    if (!camera_img || !buf || w == 0 || h == 0 || stride < w * 2) return;
    lv_canvas_set_buffer(camera_img, (void *)buf, (lv_coord_t)w, (lv_coord_t)h, LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(camera_img);
}

void ui_update_emotion(int cls, float conf)
{
    if (cls < 0 || cls > 6) return;
    int pct = (int)(conf * 100 + 0.5f);

    /* 徽章 */
    if (emotion_badge) {
        lv_obj_set_style_bg_color(emotion_badge, lv_color_hex(CLR[cls]), 0);
        lv_label_set_text(badge_name, CN[cls]);
        lv_label_set_text_fmt(badge_conf, "%d%%", pct);
        lv_obj_clear_flag(emotion_badge, LV_OBJ_FLAG_HIDDEN);
    }

    /* 右侧面板 */
    if (right_name) {
        lv_label_set_text(right_name, CN[cls]);
        lv_obj_set_style_text_color(right_name, lv_color_hex(CLR[cls]), 0);
    }
    if (right_conf) lv_label_set_text_fmt(right_conf, "%d%%", pct);
    if (right_tip) lv_label_set_text(right_tip, TIP[cls]);
}

void ui_hide_emotion(void)
{
    if (emotion_badge) lv_obj_add_flag(emotion_badge, LV_OBJ_FLAG_HIDDEN);
    if (right_name) lv_label_set_text(right_name, "---");
    if (right_conf) lv_label_set_text(right_conf, "---");
    if (right_tip) lv_label_set_text(right_tip, "请靠近摄像头");
}

void ui_update_fps(float fps)
{
    if (fps_label) lv_label_set_text_fmt(fps_label, "%.0f FPS", (double)fps);
}

void ui_set_system_status(const char *s)
{
    if (status_label && s) lv_label_set_text(status_label, s);
}

void ui_set_mode_text(const char *m)
{
    if (mode_label && m) lv_label_set_text(mode_label, m);
}

void ui_update_suggestion(const char *t)
{
    if (!suggestion_label) return;
    lv_label_set_text(suggestion_label, t ? t : "");
}

void ui_show_message(const char *title, const char *msg, uint32_t dur)
{
    if (!message_popup) return;
    if (message_timer) { lv_timer_del(message_timer); message_timer = NULL; }
    lv_obj_clean(message_popup);

    lv_obj_t *tl = lv_label_create(message_popup);
    lv_label_set_text(tl, title ? title : "");
    font_zh(tl);
    lv_obj_set_style_text_color(tl, lv_color_white(), 0);

    lv_obj_t *ml = lv_label_create(message_popup);
    lv_label_set_text(ml, msg ? msg : "");
    font_zh(ml);
    lv_obj_set_style_text_color(ml, lv_color_hex(0xC9D1D9), 0);
    lv_label_set_long_mode(ml, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ml, LV_PCT(100));

    lv_obj_clear_flag(message_popup, LV_OBJ_FLAG_HIDDEN);
    if (dur > 0) message_timer = lv_timer_create(hide_msg, dur, NULL);
}

lv_obj_t *ui_create_monitor_screen(void)
{
    ui_create_main_screen();
    return s_monitor_screen;
}
void ui_monitor_set_back_callback(lv_event_cb_t cb)
{
    if (s_back_btn) lv_obj_add_event_cb(s_back_btn, cb, LV_EVENT_CLICKED, NULL);
}

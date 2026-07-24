/* ============================================================
 * home.c — 主菜单界面实现
 *
 * 仿 Phone 风格：图标网格 + 标题 + 版本信息
 * 使用 LVGL 原生组件，无需额外框架
 * ============================================================ */

#include "home.h"
#include "ui_font_zh_22.h"
#include <string.h>

LV_FONT_DECLARE(lv_font_montserrat_14);

static void font_zh(lv_obj_t *o) { lv_obj_set_style_text_font(o, &ui_font_zh_22, 0); }

static void btn_fx(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_target(e);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED)
        lv_obj_set_style_bg_opa(b, LV_OPA_40, 0);
    else if (lv_event_get_code(e) == LV_EVENT_RELEASED || lv_event_get_code(e) == LV_EVENT_PRESS_LOST)
        lv_obj_set_style_bg_opa(b, LV_OPA_20, 0);
}

/* ---- App 图标数据 ---- */
static const char *APP_SYMBOLS[] = {
    LV_SYMBOL_EYE_OPEN,     /* 情绪监控 */
    LV_SYMBOL_ENVELOPE,     /* AI 聊天  */
    LV_SYMBOL_SETTINGS,     /* 设置    */
};
static const char *APP_NAMES[] = {
    "情绪监控", "AI 聊天", "设置",
};
static const uint32_t APP_COLORS[] = {
    0x43A047, 0x1565C0, 0x78909C,
};

#define ICON_SIZE     120
#define ICON_GAP       32
#define ICON_RADIUS    24
#define SYMBOL_SIZE    42
#define GRID_COLS       3

static lv_obj_t *s_home_screen = NULL;
static lv_obj_t *s_icon_btns[3] = {NULL};

lv_obj_t *ui_home_get_screen(void) { return s_home_screen; }

/* ---- 单个图标 ---- */
static lv_obj_t *create_app_icon(lv_obj_t *parent, int index)
{
    /* 外层按钮：圆角色块 + 居中符号 */
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, ICON_SIZE, ICON_SIZE);
    lv_obj_set_style_radius(btn, ICON_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(APP_COLORS[index]), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(APP_COLORS[index]), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    /* 符号 */
    lv_obj_t *sym = lv_label_create(btn);
    lv_label_set_text(sym, APP_SYMBOLS[index]);
    lv_obj_set_style_text_font(sym, &lv_font_montserrat_14, 0); /* LV_SYMBOL font */
    lv_obj_set_style_text_color(sym, lv_color_hex(APP_COLORS[index]), 0);
    lv_obj_center(sym);

    /* 按下效果 */
    lv_obj_add_event_cb(btn, btn_fx, LV_EVENT_ALL, NULL);

    return btn;
}

/* ---- 创建 Home 屏幕 ---- */
lv_obj_t *ui_home_create(void)
{
    s_home_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_home_screen, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_pad_all(s_home_screen, 0, 0);

    /* ===== 标题区 ===== */
    lv_obj_t *title = lv_label_create(s_home_screen);
    lv_label_set_text(title, "情绪守护");
    font_zh(title);
    lv_obj_set_style_text_color(title, lv_color_hex(0x58A6FF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    /* ===== 图标网格 ===== */
    lv_obj_t *grid = lv_obj_create(s_home_screen);
    lv_obj_set_size(grid, ICON_SIZE * GRID_COLS + ICON_GAP * (GRID_COLS - 1),
                    LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grid, ICON_GAP, 0);
    lv_obj_center(grid);

    for (int i = 0; i < 3; i++) {
        /* 图标 + 标签 的列 */
        lv_obj_t *col = lv_obj_create(grid);
        lv_obj_set_size(col, ICON_SIZE, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_set_style_pad_all(col, 0, 0);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 12, 0);

        s_icon_btns[i] = create_app_icon(col, i);

        lv_obj_t *label = lv_label_create(col);
        lv_label_set_text(label, APP_NAMES[i]);
        font_zh(label);
        lv_obj_set_style_text_color(label, lv_color_hex(0xC9D1D9), 0);
    }

    /* ===== 底部版本 ===== */
    lv_obj_t *ver = lv_label_create(s_home_screen);
    lv_label_set_text(ver, "ESP32-P4 · Emotion Guardian v1.0");
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ver, lv_color_hex(0x484F58), 0);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -16);

    return s_home_screen;
}

/* ---- 设置图标回调 ---- */
void ui_home_set_icon_callback(int icon_index, lv_event_cb_t cb)
{
    if (icon_index < 0 || icon_index > 2 || !s_icon_btns[icon_index]) return;
    lv_obj_add_event_cb(s_icon_btns[icon_index], cb, LV_EVENT_CLICKED, NULL);
}

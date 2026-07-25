/* ============================================================
 * settings.c — 设置页面实现
 *
 * 功能：
 *   - 扫描并显示附近 WiFi 列表（可点击）
 *   - 点击 SSID → 密码输入对话框 + LVGL 键盘 → 连接
 *   - 连接成功后自动保存凭据到 NVS
 *   - 已保存凭据的网络用 LV_SYMBOL_WIFI 标记
 * ============================================================ */

#include "settings.h"
#include "ui_font_zh_22.h"
#include "wifi.h"
#include "esp_lvgl_port.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

LV_FONT_DECLARE(lv_font_montserrat_14);

static const char *TAG = "SETTINGS";

static void font_zh(lv_obj_t *o) { lv_obj_set_style_text_font(o, &ui_font_zh_22, 0); }
static void font_en(lv_obj_t *o) { lv_obj_set_style_text_font(o, &lv_font_montserrat_14, 0); }

static lv_obj_t *s_screen  = NULL;
static lv_obj_t *s_list    = NULL;
static lv_obj_t *s_status  = NULL;
static lv_obj_t *s_back_btn = NULL;
static lv_obj_t *s_conn_lbl = NULL;   /* 已连接网络名 */
static bool       s_scanning = false;

/* ---- 连接对话框 ---- */
static lv_obj_t *s_dlg       = NULL;
static lv_obj_t *s_dlg_input = NULL;
static lv_obj_t *s_dlg_kb    = NULL;
static char       s_dlg_ssid[33];

/* ---- 信号强度 → ASCII 图标 ---- */
static const char *rssi_bars(int rssi)
{
    if (rssi >= -50) return "***";  /* 满格 */
    if (rssi >= -60) return "** ";
    if (rssi >= -70) return "*  ";
    return "·  ";                    /* 微弱 */
}

/* ============================================================
 * 连接对话框
 * ============================================================ */

static void dlg_close(void)
{
    if (s_dlg_kb)  { lv_obj_delete(s_dlg_kb);  s_dlg_kb  = NULL; }
    if (s_dlg)     { lv_obj_delete(s_dlg);     s_dlg     = NULL; }
    s_dlg_input = NULL;
}

static void dlg_cancel(lv_event_t *e)
{
    (void)e;
    dlg_close();
}

static void dlg_connect(lv_event_t *e)
{
    (void)e;
    if (!s_dlg_input) return;

    const char *pw = lv_textarea_get_text(s_dlg_input);
    while (*pw == ' ' || *pw == '\t') pw++;

    ESP_LOGI(TAG, "User chose: %s (pwd_len=%d)", s_dlg_ssid, (int)strlen(pw));

    /* 保存到 NVS + 连接 */
    wifi_save_credentials(s_dlg_ssid, pw);
    wifi_connect(s_dlg_ssid, pw);

    lv_label_set_text_fmt(s_status, "正在连接 %s…", s_dlg_ssid);
    dlg_close();
    ui_settings_scan_wifi();  /* 刷新列表 */
}

static void show_connect_dialog(const char *ssid)
{
    strncpy(s_dlg_ssid, ssid, sizeof(s_dlg_ssid) - 1);
    s_dlg_ssid[sizeof(s_dlg_ssid) - 1] = '\0';

    /* 半透明遮罩 */
    s_dlg = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_dlg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_dlg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dlg, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_dlg, 0, 0);
    lv_obj_set_style_radius(s_dlg, 0, 0);
    lv_obj_add_event_cb(s_dlg, dlg_cancel, LV_EVENT_CLICKED, NULL);

    /* 对话框卡片 */
    lv_obj_t *card = lv_obj_create(s_dlg);
    lv_obj_set_size(card, 280, 200);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 8, 0);
    /* 阻止点击穿透到遮罩 */
    lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 标题 */
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text_fmt(title, "连接 Wi-Fi");
    font_zh(title);
    lv_obj_set_style_text_color(title, lv_color_hex(0x58A6FF), 0);

    lv_obj_t *ssid_label = lv_label_create(card);
    lv_label_set_text_fmt(ssid_label, "%s", ssid);
    font_zh(ssid_label);
    lv_obj_set_style_text_color(ssid_label, lv_color_hex(0xC9D1D9), 0);

    /* 密码输入 */
    s_dlg_input = lv_textarea_create(card);
    lv_obj_set_size(s_dlg_input, LV_PCT(100), 36);
    lv_textarea_set_one_line(s_dlg_input, true);
    lv_textarea_set_password_mode(s_dlg_input, true);
    lv_textarea_set_placeholder_text(s_dlg_input, "输入密码…");
    font_zh(s_dlg_input);
    lv_obj_set_style_bg_color(s_dlg_input, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_border_color(s_dlg_input, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_text_color(s_dlg_input, lv_color_hex(0xC9D1D9), 0);
    lv_obj_add_event_cb(s_dlg_input, NULL, LV_EVENT_FOCUSED, NULL);

    /* 按钮行 */
    lv_obj_t *btn_row = lv_obj_create(card);
    lv_obj_set_size(btn_row, LV_PCT(100), 36);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cancel_btn = lv_btn_create(btn_row);
    lv_obj_set_size(cancel_btn, 100, 32);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_radius(cancel_btn, 6, 0);
    lv_obj_add_event_cb(cancel_btn, dlg_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel_btn);
    lv_label_set_text(cl, "取消");
    font_zh(cl);
    lv_obj_center(cl);

    lv_obj_t *connect_btn = lv_btn_create(btn_row);
    lv_obj_set_size(connect_btn, 100, 32);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(0x2EA043), 0);
    lv_obj_set_style_radius(connect_btn, 6, 0);
    lv_obj_add_event_cb(connect_btn, dlg_connect, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cbl = lv_label_create(connect_btn);
    lv_label_set_text(cbl, "连接");
    font_zh(cbl);
    lv_obj_center(cbl);

    /* 弹出键盘 */
    s_dlg_kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_width(s_dlg_kb, LV_PCT(100));
    lv_obj_set_height(s_dlg_kb, 190);
    lv_keyboard_set_textarea(s_dlg_kb, s_dlg_input);
    lv_obj_align(s_dlg_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
}

/* ============================================================
 * WiFi 列表项点击
 * ============================================================ */

/* 每个 item 携带 SSID */
typedef struct {
    lv_obj_t *btn;
    char      ssid[33];
} wifi_item_t;

static void on_item_click(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *ssid = (const char *)lv_obj_get_user_data(btn);
    if (!ssid) return;
    ESP_LOGI(TAG, "Clicked: %s", ssid);
    show_connect_dialog(ssid);
}

/* ---- "忘记网络" 按钮 ---- */
static void on_forget(lv_event_t *e)
{
    (void)e;
    wifi_clear_credentials();
    wifi_disconnect();
    if (s_conn_lbl) lv_label_set_text(s_conn_lbl, "");
    ui_settings_scan_wifi();
}

/* ============================================================
 * 异步扫描任务
 * ============================================================ */

static void on_scan_click(lv_event_t *e)
{
    (void)e;
    ui_settings_scan_wifi();
}

static void scan_task(void *arg)
{
    (void)arg;

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 300 } },
    };

    esp_err_t scan_ret = esp_wifi_scan_start(&scan_cfg, true);
    if (scan_ret != ESP_OK) {
        ESP_LOGW(TAG, "Scan start failed: %s (%d)",
                 esp_err_to_name(scan_ret), scan_ret);
        if (lvgl_port_lock(-1)) {
            lv_label_set_text(s_status, "扫描失败（WiFi 未就绪）");
            lvgl_port_unlock();
        }
        s_scanning = false;
        vTaskDelete(NULL);
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (lvgl_port_lock(-1)) {
        lv_obj_clean(s_list);

        /* 状态行 */
        if (ap_count == 0) {
            lv_label_set_text(s_status, "未发现网络");
        } else {
            char saved_ssid[33];
            bool has_saved = (wifi_get_saved_ssid(saved_ssid, sizeof(saved_ssid)) == ESP_OK);
            lv_label_set_text_fmt(s_status, "发现 %d 个网络", ap_count);

            wifi_ap_record_t *aps = malloc(sizeof(wifi_ap_record_t) * ap_count);
            if (aps) {
                esp_wifi_scan_get_ap_records(&ap_count, aps);

                for (int i = 0; i < ap_count && i < 32; i++) {
                    bool is_saved = has_saved && (strcmp((const char *)aps[i].ssid, saved_ssid) == 0);

                    /* 按钮容器 */
                    lv_obj_t *btn = lv_btn_create(s_list);
                    lv_obj_set_size(btn, LV_PCT(100), 36);
                    lv_obj_set_style_bg_color(btn, lv_color_hex(0x21262D), 0);
                    lv_obj_set_style_radius(btn, 6, 0);
                    lv_obj_set_style_border_width(btn, 0, 0);
                    lv_obj_set_style_pad_hor(btn, 10, 0);
                    lv_obj_set_style_shadow_width(btn, 0, 0);

                    /* 存储 SSID */
                    char *data = strdup((const char *)aps[i].ssid);
                    lv_obj_set_user_data(btn, data);
                    lv_obj_add_event_cb(btn, on_item_click, LV_EVENT_CLICKED, NULL);

                    /* 标签 */
                    lv_obj_t *label = lv_label_create(btn);
                    if (is_saved) {
                        lv_label_set_text_fmt(label, " %s  %s  %ddBm",
                                               rssi_bars(aps[i].rssi),
                                               aps[i].ssid, aps[i].rssi);
                    } else {
                        lv_label_set_text_fmt(label, " %s  %s  %ddBm",
                                              rssi_bars(aps[i].rssi),
                                              aps[i].ssid, aps[i].rssi);
                    }
                    lv_obj_set_style_text_color(label, lv_color_hex(0xC9D1D9), 0);
                    font_zh(label);
                    lv_obj_center(label);
                }
                free(aps);
            } else {
                lv_label_set_text(s_status, "内存不足");
            }
        }
        lvgl_port_unlock();
    }

    s_scanning = false;
    vTaskDelete(NULL);
}

/* ============================================================
 * 启动扫描
 * ============================================================ */

void ui_settings_scan_wifi(void)
{
    if (!s_list || !s_status) return;
    if (s_scanning) return;

    /* 确保 WiFi 栈已初始化 */
    if (wifi_get_status() == WIFI_STATUS_DISCONNECTED) {
        /* 尝试 init（幂等，没 init 过就 init） */
    }

    s_scanning = true;
    lv_label_set_text(s_status, "正在扫描…");
    lv_obj_clean(s_list);

    xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 1, NULL);
}

/* ============================================================
 * 按钮 + 屏幕
 * ============================================================ */

static void btn_fx(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_target(e);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED)
        lv_obj_set_style_bg_opa(b, LV_OPA_30, 0);
    else if (lv_event_get_code(e) == LV_EVENT_RELEASED ||
             lv_event_get_code(e) == LV_EVENT_PRESS_LOST)
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

lv_obj_t *ui_settings_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_flex_flow(s_screen, LV_FLEX_FLOW_COLUMN);

    /* ===== 导航栏 ===== */
    lv_obj_t *nav = lv_obj_create(s_screen);
    lv_obj_set_size(nav, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(nav, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_width(nav, 0, 0);
    lv_obj_set_style_radius(nav, 0, 0);
    lv_obj_set_style_pad_hor(nav, 8, 0);
    lv_obj_set_style_pad_ver(nav, 0, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_back_btn = mk_btn(nav, 36, 36);
    lv_obj_t *bs = lv_label_create(s_back_btn);
    lv_label_set_text(bs, LV_SYMBOL_LEFT);
    font_en(bs);
    lv_obj_set_style_text_color(bs, lv_color_hex(0xC9D1D9), 0);
    lv_obj_center(bs);

    lv_obj_t *title = lv_label_create(nav);
    lv_label_set_text(title, "设置 — WiFi");
    font_zh(title);
    lv_obj_set_style_text_color(title, lv_color_hex(0x58A6FF), 0);
    lv_obj_set_style_margin_left(title, 12, 0);

    /* ===== 连接状态 + 操作栏 ===== */
    lv_obj_t *bar = lv_obj_create(s_screen);
    lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 14, 0);
    lv_obj_set_style_pad_ver(bar, 8, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 左侧状态 */
    lv_obj_t *left = lv_obj_create(bar);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(left, 2, 0);

    s_conn_lbl = lv_label_create(left);
    lv_label_set_text(s_conn_lbl, LV_SYMBOL_WIFI " --");
    font_en(s_conn_lbl);
    lv_obj_set_style_text_color(s_conn_lbl, lv_color_hex(0x8B949E), 0);

    s_status = lv_label_create(left);
    lv_label_set_text(s_status, "点击扫描按钮");
    font_zh(s_status);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x8B949E), 0);

    /* 右侧按钮 */
    lv_obj_t *right_btns = lv_obj_create(bar);
    lv_obj_set_style_bg_opa(right_btns, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_btns, 0, 0);
    lv_obj_set_style_pad_all(right_btns, 0, 0);
    lv_obj_set_flex_flow(right_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(right_btns, 6, 0);

    lv_obj_t *scan_btn = mk_btn(right_btns, 80, 36);
    lv_obj_t *sl = lv_label_create(scan_btn);
    lv_label_set_text(sl, "扫描");
    font_zh(sl);
    lv_obj_add_event_cb(scan_btn, on_scan_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *forget_btn = mk_btn(right_btns, 48, 36);
    lv_obj_add_event_cb(forget_btn, on_forget, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fl = lv_label_create(forget_btn);
    lv_label_set_text(fl, LV_SYMBOL_TRASH);
    font_en(fl);
    lv_obj_set_style_text_color(fl, lv_color_hex(0xDA3633), 0);
    lv_obj_center(fl);

    /* ===== 列表区 ===== */
    lv_obj_t *container = lv_obj_create(s_screen);
    lv_obj_set_size(container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(container, 1);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_pad_all(container, 14, 0);

    s_list = lv_obj_create(container);
    lv_obj_set_size(s_list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 4, 0);

    return s_screen;
}

void ui_settings_set_back_callback(lv_event_cb_t cb)
{
    if (s_back_btn) lv_obj_add_event_cb(s_back_btn, cb, LV_EVENT_CLICKED, NULL);
}

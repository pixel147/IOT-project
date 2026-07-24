/* ============================================================
 * settings.c — 设置页面实现
 *
 * 功能：扫描并显示附近 WiFi 列表（异步，不阻塞 LVGL）
 * ============================================================ */

#include "settings.h"
#include "ui_font_zh_22.h"
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

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_status = NULL;
static lv_obj_t *s_back_btn = NULL;
static bool s_scanning = false;

static void on_scan_click(lv_event_t *e)
{
    (void)e;
    ui_settings_scan_wifi();
}

/* ============================================================
 * 异步扫描：独立 FreeRTOS 任务，避免阻塞 LVGL
 * ============================================================ */
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

    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        ESP_LOGW(TAG, "Scan start failed");
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

        if (ap_count == 0) {
            lv_label_set_text(s_status, "未发现 WiFi 网络");
        } else {
            wifi_ap_record_t *aps = malloc(sizeof(wifi_ap_record_t) * ap_count);
            if (aps) {
                esp_wifi_scan_get_ap_records(&ap_count, aps);
                lv_label_set_text_fmt(s_status, "发现 %d 个网络", ap_count);

                char buf[80];
                for (int i = 0; i < ap_count && i < 32; i++) {
                    const char *bars;
                    if (aps[i].rssi >= -50)      bars = "▂▄▆█";
                    else if (aps[i].rssi >= -60) bars = "▂▄▆ ";
                    else if (aps[i].rssi >= -70) bars = "▂▄  ";
                    else                          bars = "▂   ";

                    snprintf(buf, sizeof(buf), " %s  %s  %ddBm",
                             bars, (const char *)aps[i].ssid, aps[i].rssi);

                    lv_obj_t *item = lv_label_create(s_list);
                    lv_label_set_text(item, buf);
                    lv_obj_set_width(item, LV_PCT(100));
                    lv_obj_set_style_text_color(item, lv_color_hex(0xC9D1D9), 0);
                    font_en(item);
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
    if (s_scanning) return; /* 防止重复点 */

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
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_flex_flow(s_scr, LV_FLEX_FLOW_COLUMN);

    /* ===== 导航栏 ===== */
    lv_obj_t *nav = lv_obj_create(s_scr);
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
    lv_obj_set_style_text_color(bs, lv_color_hex(0xC9D1D9), 0);
    lv_obj_center(bs);

    lv_obj_t *title = lv_label_create(nav);
    lv_label_set_text(title, "设置 — WiFi");
    font_zh(title);
    lv_obj_set_style_text_color(title, lv_color_hex(0x58A6FF), 0);
    lv_obj_set_style_margin_left(title, 12, 0);

    /* ===== 操作栏 ===== */
    lv_obj_t *bar = lv_obj_create(s_scr);
    lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 14, 0);
    lv_obj_set_style_pad_ver(bar, 8, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_status = lv_label_create(bar);
    lv_label_set_text(s_status, "点击扫描按钮");
    font_zh(s_status);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x8B949E), 0);

    lv_obj_t *scan_btn = mk_btn(bar, 80, 36);
    lv_obj_t *sl = lv_label_create(scan_btn);
    lv_label_set_text(sl, LV_SYMBOL_REFRESH " 扫描");
    lv_obj_set_style_text_color(sl, lv_color_hex(0xC9D1D9), 0);
    font_zh(sl);
    lv_obj_add_event_cb(scan_btn, on_scan_click, LV_EVENT_CLICKED, NULL);

    /* ===== 列表区 ===== */
    lv_obj_t *container = lv_obj_create(s_scr);
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

    return s_scr;
}

void ui_settings_set_back_callback(lv_event_cb_t cb)
{
    if (s_back_btn) lv_obj_add_event_cb(s_back_btn, cb, LV_EVENT_CLICKED, NULL);
}

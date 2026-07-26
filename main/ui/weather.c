/* ============================================================
 * weather.c — 天气模块实现
 *
 * 数据源：Open-Meteo 免费 API（无需 API Key）
 * ============================================================ */

#include "weather.h"
#include "ui_font_zh_22.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_lvgl_port.h"
#include "cJSON.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

LV_FONT_DECLARE(lv_font_montserrat_14);

static const char *TAG = "WEATHER";

#define NVS_NS      "weather"
#define KEY_LAT     "lat"
#define KEY_LON     "lon"
#define KEY_CITY    "city"
#define REFRESH_MS  (30 * 60 * 1000)

static char s_lat[16] = "39.9042";
static char s_lon[16] = "116.4074";
static char s_city[64] = "\xe5\x8c\x97\xe4\xba\xac"; /* 北京 */

static SemaphoreHandle_t s_mutex = NULL;
static volatile weather_data_t s_weather = {0};
static volatile bool s_refresh = false;

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_back = NULL;
static lv_obj_t *s_temp = NULL;
static lv_obj_t *s_cond = NULL;
static lv_obj_t *s_loc = NULL;
static lv_obj_t *s_humi = NULL;
static lv_obj_t *s_wind = NULL;
static lv_obj_t *s_city_dlg = NULL;

static const char *W_NAME[] = {
    "\xe5\x8c\x97\xe4\xba\xac","\xe4\xb8\x8a\xe6\xb5\xb7","\xe5\xb9\xbf\xe5\xb7\x9e","\xe6\xb7\xb1\xe5\x9c\xb3","\xe6\x9d\xad\xe5\xb7\x9e",
    "\xe6\x88\x90\xe9\x83\xbd","\xe6\xad\xa6\xe6\xb1\x89","\xe5\x8d\x97\xe4\xba\xac","\xe9\x87\x8d\xe5\xba\x86","\xe8\xa5\xbf\xe5\xae\x89",
    "\xe5\xa4\xa9\xe6\xb4\xa5","\xe8\x8b\x8f\xe5\xb7\x9e","\xe9\x95\xbf\xe6\xb2\x99","\xe9\x83\x91\xe5\xb7\x9e","\xe4\xb8\x9c\xe8\x8e\x9e",
    "\xe9\x9d\x92\xe5\xb2\x9b","\xe6\xb2\x88\xe9\x98\xb3","\xe5\xae\x81\xe6\xb3\xa2","\xe6\x98\x86\xe6\x98\x8e","\xe5\xa4\xa7\xe8\xbf\x9e",
    "\xe7\xa7\xa6\xe7\x9a\x87\xe5\xb2\x9b",
};
static const char *W_LAT[] = {
    "39.9042","31.2304","23.1291","22.5431","30.2741","30.5728","30.5928","32.0603","29.4316","34.3416",
    "39.1252","31.2990","28.2282","34.7466","23.0463","36.0818","41.8057","29.8683","25.0389","38.9140","39.9316",
};
static const char *W_LON[] = {
    "116.4074","121.4737","113.2644","114.0579","120.1551","104.0668","114.3055","118.7969","106.9123","108.9398",
    "117.1910","120.6222","112.9332","113.6253","113.7518","120.3826","123.4667","121.5439","102.7180","121.6147","119.5996",
};
#define W_NUM (sizeof(W_NAME)/sizeof(W_NAME[0]))

static void f_zh(lv_obj_t *o) { lv_obj_set_style_text_font(o, &ui_font_zh_22, 0); }
static void f_en(lv_obj_t *o) { lv_obj_set_style_text_font(o, &lv_font_montserrat_14, 0); }

static void btn_effect(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_target(e);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED)
        lv_obj_set_style_bg_opa(b, LV_OPA_30, 0);
    else if (lv_event_get_code(e) == LV_EVENT_RELEASED || lv_event_get_code(e) == LV_EVENT_PRESS_LOST)
        lv_obj_set_style_bg_opa(b, LV_OPA_0, 0);
}

/* ---- WMO weather code -> Chinese ---- */
static const char *wmo_str(int code)
{
    if (code == 0) return "\xe6\x99\xb4";
    if (code <= 3) return "\xe5\xa4\x9a\xe4\xba\x91";
    if (code <= 48) return "\xe9\x9b\xbe";
    if (code <= 57) return "\xe9\x9b\xa8";
    if (code <= 77) return "\xe9\x9b\xaa";
    if (code <= 86) return "\xe9\x98\xb5\xe9\x9b\xa8";
    if (code <= 99) return "\xe9\x9b\xb7\xe6\x9a\xb4";
    return "\xe6\x9c\xaa\xe7\x9f\xa5";
}

/* ---- HTTP callback ---- */
static esp_err_t http_cb(esp_http_client_event_t *evt)
{
    static char *buf = NULL;
    static int len = 0;

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (!buf) { buf = malloc(2048); len = 0; }
        int cpy = evt->data_len;
        if (len + cpy > 2047) cpy = 2047 - len;
        if (cpy > 0) { memcpy(buf + len, evt->data, cpy); len += cpy; buf[len] = 0; }
    }
    if (evt->event_id == HTTP_EVENT_ON_FINISH && buf && len > 0) {
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
            if (cJSON_IsObject(cur)) {
                weather_data_t w = {0};
                time_t now; time(&now); w.last_update = now * 1000000;
                w.condition = WEATHER_UNKNOWN;
                cJSON *v;
                v = cJSON_GetObjectItemCaseSensitive(cur, "temperature_2m");
                if (cJSON_IsNumber(v)) w.temperature = (float)v->valuedouble;
                v = cJSON_GetObjectItemCaseSensitive(cur, "relative_humidity_2m");
                if (cJSON_IsNumber(v)) w.humidity = v->valueint;
                v = cJSON_GetObjectItemCaseSensitive(cur, "wind_speed_10m");
                if (cJSON_IsNumber(v)) w.wind_speed = (float)v->valuedouble;
                v = cJSON_GetObjectItemCaseSensitive(cur, "weather_code");
                if (cJSON_IsNumber(v)) {
                    int code = v->valueint;
                    if (code == 0) w.condition = WEATHER_CLEAR;
                    else if (code <= 3) w.condition = WEATHER_PARTLY_CLOUDY;
                    else if (code <= 48) w.condition = WEATHER_FOG;
                    else if (code <= 57) w.condition = WEATHER_RAIN;
                    else if (code <= 77) w.condition = WEATHER_SNOW;
                    else if (code <= 86) w.condition = WEATHER_RAIN;
                    else if (code <= 99) w.condition = WEATHER_THUNDERSTORM;
                }
                strlcpy(w.location, s_city, sizeof(w.location));
                if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_weather = w;
                if (s_mutex) xSemaphoreGive(s_mutex);

                /* Update UI */
                int t = (int)roundf(w.temperature);
                const char *cond = "";
                uint32_t cond_clr = 0x78909C;
                switch (w.condition) {
                    case WEATHER_CLEAR:         cond = "\xe6\x99\xb4"; cond_clr = 0xFDD835; break;
                    case WEATHER_PARTLY_CLOUDY: cond = "\xe5\xa4\x9a\xe4\xba\x91"; cond_clr = 0x90A4AE; break;
                    case WEATHER_CLOUDY:        cond = "\xe9\x98\xb4"; cond_clr = 0x78909C; break;
                    case WEATHER_FOG:           cond = "\xe9\x9b\xbe"; cond_clr = 0xB0BEC5; break;
                    case WEATHER_RAIN:          cond = "\xe9\x9b\xa8"; cond_clr = 0x1565C0; break;
                    case WEATHER_SNOW:          cond = "\xe9\x9b\xaa"; cond_clr = 0xE0E0E0; break;
                    case WEATHER_THUNDERSTORM:  cond = "\xe9\x9b\xb7\xe6\x9a\xb4"; cond_clr = 0xE53935; break;
                    default:                    cond = "?"; break;
                }
                if (lvgl_port_lock(-1)) {
                    if (s_temp) lv_label_set_text_fmt(s_temp, "%d C", t);
                    if (s_loc) lv_label_set_text(s_loc, s_city);
                    if (s_cond) {
                        lv_label_set_text(s_cond, cond);
                        lv_obj_set_style_text_color(s_cond, lv_color_hex(cond_clr), 0);
                    }
                    if (s_humi) lv_label_set_text_fmt(s_humi, "\xe6\xb9\xbf\xe5\xba\xa6  %d%%", w.humidity);
                    if (s_wind) lv_label_set_text_fmt(s_wind, "\xe9\xa3\x8e\xe9\x80\x9f  %.1f km/h", (double)w.wind_speed);
                    lvgl_port_unlock();
                }
            }
            cJSON_Delete(root);
        }
        free(buf); buf = NULL; len = 0;
    }
    if (evt->event_id == HTTP_EVENT_ERROR || evt->event_id == HTTP_EVENT_DISCONNECTED) {
        if (buf) { free(buf); buf = NULL; len = 0; }
    }
    return ESP_OK;
}

/* ---- Fetch task with retry ---- */
static void fetch_task(void *arg)
{
    (void)arg;
    char url[512];
    vTaskDelay(pdMS_TO_TICKS(30000));

    /* Load saved city from NVS */
    {
        nvs_handle_t nv;
        char buf[64] = {};
        if (nvs_open(NVS_NS, NVS_READONLY, &nv) == ESP_OK) {
            size_t sz = sizeof(buf);
            if (nvs_get_str(nv, KEY_CITY, buf, &sz) == ESP_OK && buf[0]) {
                sz = sizeof(s_lat);
                if (nvs_get_str(nv, KEY_LAT, s_lat, &sz) == ESP_OK) {
                    sz = sizeof(s_lon);
                    nvs_get_str(nv, KEY_LON, s_lon, &sz);
                    strcpy(s_city, buf);
                }
            }
            nvs_close(nv);
        }
    }

    while (1) {
        snprintf(url, sizeof(url),
                 "https://api.open-meteo.com/v1/forecast"
                 "?latitude=%s&longitude=%s"
                 "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
                 "&timezone=auto", s_lat, s_lon);

        for (int retry = 0; retry < 3; retry++) {
            esp_http_client_config_t cfg = {
                .url = url, .method = HTTP_METHOD_GET,
                .timeout_ms = 15000, .event_handler = http_cb,
                .crt_bundle_attach = esp_crt_bundle_attach,
            };
            esp_http_client_handle_t c = esp_http_client_init(&cfg);
            if (!c) break;
            esp_err_t err = esp_http_client_perform(c);
            esp_http_client_cleanup(c);
            if (err == ESP_OK) break;
            ESP_LOGW(TAG, "Retry %d/3: %s", retry + 1, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(3000));
        }

        for (int i = 0; i < 1800 && !s_refresh; i++) vTaskDelay(pdMS_TO_TICKS(1000));
        s_refresh = false;
    }
}

/* ---- Public API ---- */

bool weather_get_current(weather_data_t *out)
{
    if (!out) return false;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_weather;
    if (s_mutex) xSemaphoreGive(s_mutex);
    return true;
}

void weather_request_refresh(void) { s_refresh = true; }

void weather_set_city_coord(const char *city, const char *lat, const char *lon)
{
    if (city) strcpy(s_city, city);
    if (lat)  strcpy(s_lat, lat);
    if (lon)  strcpy(s_lon, lon);
    s_refresh = true;
    /* Save to NVS */
    nvs_handle_t nv;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nv) == ESP_OK) {
        nvs_set_str(nv, KEY_CITY, s_city);
        nvs_set_str(nv, KEY_LAT, s_lat);
        nvs_set_str(nv, KEY_LON, s_lon);
        nvs_commit(nv);
        nvs_close(nv);
    }
}

static void on_refresh(lv_event_t *e)
{
    (void)e;
    weather_request_refresh();
}

/* ---- City picker dialog ---- */
static void dlg_cancel(lv_event_t *e) { (void)e; if (s_city_dlg) { lv_obj_delete(s_city_dlg); s_city_dlg = NULL; } }

static void dlg_select(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (idx >= 0 && (size_t)idx < W_NUM) {
        weather_set_city_coord(W_NAME[idx], W_LAT[idx], W_LON[idx]);
        if (s_loc) lv_label_set_text(s_loc, W_NAME[idx]);
    }
    if (s_city_dlg) { lv_obj_delete(s_city_dlg); s_city_dlg = NULL; }
}

static void show_picker(lv_event_t *e)
{
    (void)e;
    if (s_city_dlg) { lv_obj_delete(s_city_dlg); s_city_dlg = NULL; }
    s_city_dlg = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_city_dlg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_city_dlg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_city_dlg, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_city_dlg, 0, 0);
    lv_obj_set_style_radius(s_city_dlg, 0, 0);
    lv_obj_add_event_cb(s_city_dlg, dlg_cancel, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_city_dlg);
    lv_obj_set_size(card, 340, 400);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_shadow_width(card, 16, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "\xe9\x80\x89\xe6\x8b\xa9\xe5\x9f\x8e\xe5\xb8\x82");
    f_zh(title);
    lv_obj_set_style_text_color(title, lv_color_hex(0x58A6FF), 0);

    lv_obj_t *list = lv_obj_create(card);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_style_pad_column(list, 6, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    for (size_t i = 0; i < W_NUM; i++) {
        lv_obj_t *b = lv_btn_create(list);
        lv_obj_set_size(b, LV_SIZE_CONTENT, 44);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x21262D), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x30363D), 0);
        lv_obj_set_style_pad_hor(b, 14, 0);
        lv_obj_set_user_data(b, (void*)(intptr_t)i);
        lv_obj_add_event_cb(b, dlg_select, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lb = lv_label_create(b);
        lv_label_set_text(lb, W_NAME[i]);
        f_zh(lb);
        lv_obj_set_style_text_color(lb, lv_color_hex(0xC9D1D9), 0);
        lv_obj_center(lb);
    }
}

/* ---- Create weather screen ---- */
lv_obj_t *ui_weather_create(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_flex_flow(s_scr, LV_FLEX_FLOW_COLUMN);

    /* ===== Navbar (same style as other screens) ===== */
    lv_obj_t *nav = lv_obj_create(s_scr);
    lv_obj_set_size(nav, LV_PCT(100), 68);
    lv_obj_set_style_bg_color(nav, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_width(nav, 1, 0);
    lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(nav, lv_color_hex(0x21262D), 0);
    lv_obj_set_style_radius(nav, 0, 0);
    lv_obj_set_style_pad_hor(nav, 10, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_back = lv_btn_create(nav);
    lv_obj_set_size(s_back, 56, 56);
    lv_obj_set_style_bg_color(s_back, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(s_back, 0, 0);
    lv_obj_set_style_radius(s_back, 10, 0);
    lv_obj_set_style_shadow_width(s_back, 0, 0);
    lv_obj_add_event_cb(s_back, btn_effect, LV_EVENT_ALL, NULL);
    lv_obj_t *bs = lv_label_create(s_back);
    lv_label_set_text(bs, LV_SYMBOL_LEFT);
    f_en(bs);
    lv_obj_set_style_text_color(bs, lv_color_hex(0xC9D1D9), 0);
    lv_obj_center(bs);

    lv_obj_t *ti = lv_label_create(nav);
    lv_label_set_text(ti, "\xe5\xa4\xa9\xe6\xb0\x94");
    f_zh(ti);
    lv_obj_set_style_text_color(ti, lv_color_hex(0x58A6FF), 0);
    lv_obj_set_style_margin_left(ti, 12, 0);

    /* ===== Main content, centered ===== */
    lv_obj_t *body = lv_obj_create(s_scr);
    lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Temperature - very large, centered */
    s_temp = lv_label_create(body);
    lv_label_set_text(s_temp, "-- C");
    lv_obj_set_style_text_font(s_temp, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_temp, lv_color_hex(0xFDD835), 0);
    lv_obj_set_style_margin_top(s_temp, 30, 0);

    /* Weather condition */
    s_cond = lv_label_create(body);
    lv_label_set_text(s_cond, "--");
    f_zh(s_cond);
    lv_obj_set_style_text_color(s_cond, lv_color_hex(0xC9D1D9), 0);
    lv_obj_set_style_margin_top(s_cond, 14, 0);

    /* City name */
    s_loc = lv_label_create(body);
    lv_label_set_text(s_loc, s_city);
    f_zh(s_loc);
    lv_obj_set_style_text_color(s_loc, lv_color_hex(0x8B949E), 0);
    lv_obj_set_style_margin_top(s_loc, 6, 0);

    /* Separator line */
    lv_obj_t *line = lv_obj_create(body);
    lv_obj_set_size(line, 80, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_50, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_margin_top(line, 20, 0);
    lv_obj_set_style_margin_bottom(line, 16, 0);

    /* Details: two lines, centered, with proper labels and units */
    s_humi = lv_label_create(body);
    lv_label_set_text(s_humi, "\xe6\xb9\xbf\xe5\xba\xa6  --%");
    f_zh(s_humi);
    lv_obj_set_style_text_color(s_humi, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_margin_top(s_humi, 6, 0);

    s_wind = lv_label_create(body);
    lv_label_set_text(s_wind, "\xe9\xa3\x8e\xe9\x80\x9f  -- km/h");
    f_zh(s_wind);
    lv_obj_set_style_text_color(s_wind, lv_color_hex(0xB0BEC5), 0);
    lv_obj_set_style_margin_top(s_wind, 6, 0);

    /* ===== Bottom bar ===== */
    lv_obj_t *ctrl = lv_obj_create(s_scr);
    lv_obj_set_size(ctrl, LV_PCT(100), 68);
    lv_obj_set_style_bg_color(ctrl, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_width(ctrl, 1, 0);
    lv_obj_set_style_border_side(ctrl, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(ctrl, lv_color_hex(0x21262D), 0);
    lv_obj_set_style_radius(ctrl, 0, 0);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *ref = lv_btn_create(ctrl);
    lv_obj_set_size(ref, 140, 56);
    lv_obj_set_style_bg_color(ref, lv_color_hex(0x2EA043), 0);
    lv_obj_set_style_radius(ref, 10, 0);
    lv_obj_set_style_border_width(ref, 0, 0);
    lv_obj_add_event_cb(ref, on_refresh, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(ref);
    lv_label_set_text(rl, "\xe5\x88\xb7\xe6\x96\xb0");
    f_zh(rl);
    lv_obj_set_style_text_color(rl, lv_color_white(), 0);
    lv_obj_center(rl);

    lv_obj_t *city = lv_btn_create(ctrl);
    lv_obj_set_size(city, 140, 56);
    lv_obj_set_style_bg_color(city, lv_color_hex(0x1565C0), 0);
    lv_obj_set_style_radius(city, 10, 0);
    lv_obj_set_style_border_width(city, 0, 0);
    lv_obj_set_style_margin_left(city, 24, 0);
    lv_obj_add_event_cb(city, show_picker, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(city);
    lv_label_set_text(cl, "\xe5\x9f\x8e\xe5\xb8\x82");
    f_zh(cl);
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_center(cl);

    /* Start fetch task */
    xTaskCreate(fetch_task, "weather_fetch", 8192, NULL, 1, NULL);

    return s_scr;
}

void ui_weather_set_back_callback(lv_event_cb_t cb)
{
    if (s_back) lv_obj_add_event_cb(s_back, cb, LV_EVENT_CLICKED, NULL);
}
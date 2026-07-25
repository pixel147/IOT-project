#include "chat.h"
#include "llm_client.h"
#include "esp_lvgl_port.h"
#include "ui_font_zh_22.h"
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_montserrat_14);

#define CHAT_MAX_MESSAGES 40
#define CHAT_RENDER_TEXT_MAX 4096

static lv_obj_t *s_screen;
static lv_obj_t *s_messages;
static lv_obj_t *s_input;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_send_btn;
static lv_obj_t *s_voice_btn;
static lv_obj_t *s_clear_btn;
static lv_obj_t *s_status;
static lv_obj_t *s_back_btn;
static lv_obj_t *s_emo_badge;
static lv_obj_t *s_emo_label;
static ui_chat_voice_start_cb_t s_voice_start_cb;

/* 情绪配色（与 ui.c 一致） */
static const uint32_t EMO_COLORS[7] = {
    0xE53935, 0x2E7D32, 0x1565C0, 0x43A047, 0xE53935, 0xFDD835, 0x78909C
};
static const char *EMO_NAMES[7] = {
    "生气", "厌恶", "害怕", "开心", "难过", "惊讶", "平静"
};

/* 更新情绪徽章（每次发消息前调用） */
static void update_emotion_badge(void)
{
    int cls;
    float conf;
    if (!s_emo_badge || !s_emo_label) return;

    if (llm_get_last_emotion(&cls, &conf)) {
        int pct = (int)(conf * 100.0f + 0.5f);
        if (pct > 100) pct = 100;
        if (cls < 0) cls = 0; else if (cls > 6) cls = 6;

        lv_label_set_text_fmt(s_emo_label, "%s %d%%", EMO_NAMES[cls], pct);
        lv_obj_set_style_bg_color(s_emo_badge, lv_color_hex(EMO_COLORS[cls]), 0);
        lv_obj_clear_flag(s_emo_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_emo_badge, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_font(lv_obj_t *obj)
{
    lv_obj_set_style_text_font(obj, &ui_font_zh_22, 0);
}

static size_t utf8_decode(const char *text, uint32_t *codepoint)
{
    const uint8_t *s = (const uint8_t *)text;
    if (s[0] < 0x80) {
        *codepoint = s[0];
        return 1;
    }
    if ((s[0] & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) {
        *codepoint = ((uint32_t)(s[0] & 0x1f) << 6) | (s[1] & 0x3f);
        return *codepoint >= 0x80 ? 2 : 0;
    }
    if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
        *codepoint = ((uint32_t)(s[0] & 0x0f) << 12) |
                     ((uint32_t)(s[1] & 0x3f) << 6) | (s[2] & 0x3f);
        return (*codepoint >= 0x800 && (*codepoint < 0xd800 || *codepoint > 0xdfff)) ? 3 : 0;
    }
    if ((s[0] & 0xf8) == 0xf0 && (s[1] & 0xc0) == 0x80 &&
        (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
        *codepoint = ((uint32_t)(s[0] & 0x07) << 18) |
                     ((uint32_t)(s[1] & 0x3f) << 12) |
                     ((uint32_t)(s[2] & 0x3f) << 6) | (s[3] & 0x3f);
        return (*codepoint >= 0x10000 && *codepoint <= 0x10ffff) ? 4 : 0;
    }
    return 0;
}

static void sanitize_display_text(char *out, size_t out_size, const char *text)
{
    const char *src = text;
    char *dst = out;
    char *end = out + out_size - 1;

    while (*src && dst < end) {
        if (*src == '\r') {
            src++;
            continue;
        }
        if (*src == '\n') {
            *dst++ = *src++;
            continue;
        }

        uint32_t codepoint = 0;
        size_t bytes = utf8_decode(src, &codepoint);
        lv_font_glyph_dsc_t glyph_dsc;
        if (bytes && dst + bytes <= end &&
            lv_font_get_glyph_dsc(&ui_font_zh_22, &glyph_dsc, codepoint, 0)) {
            memcpy(dst, src, bytes);
            dst += bytes;
            src += bytes;
        } else {
            *dst++ = '?';
            src += bytes ? bytes : 1;
        }
    }
    *dst = '\0';
}

static void set_request_state(bool busy)
{
    if (busy) {
        lv_obj_add_state(s_send_btn, LV_STATE_DISABLED);
        lv_obj_add_state(s_clear_btn, LV_STATE_DISABLED);
        lv_label_set_text(s_status, "Thinking...");
    } else {
        lv_obj_remove_state(s_send_btn, LV_STATE_DISABLED);
        lv_obj_remove_state(s_clear_btn, LV_STATE_DISABLED);
        lv_label_set_text(s_status, "Ready");
    }
}

static void append_message(const char *speaker, const char *text, uint32_t color)
{
    if (!s_messages || !text) return;

    size_t display_size = strnlen(text, CHAT_RENDER_TEXT_MAX - 1) + 1;
    char *display_text = malloc(display_size);
    if (!display_text) return;
    sanitize_display_text(display_text, display_size, text);

    while (lv_obj_get_child_count(s_messages) >= CHAT_MAX_MESSAGES) {
        lv_obj_delete(lv_obj_get_child(s_messages, 0));
    }

    /* 消息气泡容器 */
    lv_obj_t *bubble = lv_obj_create(s_messages);
    lv_obj_set_size(bubble, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_70, 0);
    lv_obj_set_style_radius(bubble, 8, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_hor(bubble, 12, 0);
    lv_obj_set_style_pad_ver(bubble, 6, 0);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(bubble, 2, 0);

    /* 说话者标签 */
    lv_obj_t *who = lv_label_create(bubble);
    lv_label_set_text(who, speaker);
    set_font(who);
    lv_obj_set_style_text_color(who, lv_color_hex(color), 0);

    /* 消息正文 */
    lv_obj_t *msg = lv_label_create(bubble);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, LV_PCT(100));
    lv_label_set_text(msg, display_text);
    set_font(msg);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xC9D1D9), 0);

    lv_obj_scroll_to_view(bubble, LV_ANIM_OFF);
    free(display_text);
}

static void on_llm_response(const char *response, bool success,
                            int http_status, void *user_data)
{
    (void)http_status;
    (void)user_data;

    if (lvgl_port_lock(-1)) {
        append_message("Assistant", success && response ? response : "Request failed", 0xC9D1D9);
        set_request_state(false);
        lvgl_port_unlock();
    }
}

static void send_message(void)
{
    const char *text = lv_textarea_get_text(s_input);
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    if (*text == '\0' || llm_is_busy()) return;

    set_request_state(true);
    esp_err_t ret = llm_chat_async(text, on_llm_response, NULL);
    append_message("You", text, 0x58A6FF);
    lv_textarea_set_text(s_input, "");
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    if (ret != ESP_OK) {
        append_message("Assistant", "LLM is not ready", 0xF85149);
        set_request_state(false);
    }
}

static void on_send(lv_event_t *event)
{
    (void)event;
    update_emotion_badge();  /* 发消息前刷新情绪，让用户看到 AI 将基于什么情绪回复 */
    send_message();
}

static void on_voice_start(lv_event_t *event)
{
    (void)event;
    if (s_voice_start_cb) s_voice_start_cb();
}

static void on_clear(lv_event_t *event)
{
    (void)event;
    if (llm_is_busy()) return;

    llm_clear_history();
    lv_obj_clean(s_messages);
    lv_label_set_text(s_status, "History cleared");
}

static void on_input(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_FOCUSED) {
        lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_keyboard(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_READY) send_message();
}

static void button_style(lv_obj_t *button)
{
    lv_obj_set_style_bg_color(button, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x484F58), 0);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
}

static lv_obj_t *icon_button(lv_obj_t *parent, const char *icon)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, 56, 56);
    button_style(button);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, icon);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xC9D1D9), 0);
    lv_obj_center(label);
    return button;
}

lv_obj_t *ui_chat_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_flex_flow(s_screen, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *nav = lv_obj_create(s_screen);
    lv_obj_set_size(nav, LV_PCT(100), 68);
    lv_obj_set_style_bg_color(nav, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_width(nav, 0, 0);
    lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(nav, 1, 0);
    lv_obj_set_style_border_color(nav, lv_color_hex(0x21262D), 0);
    lv_obj_set_style_radius(nav, 0, 0);
    lv_obj_set_style_pad_hor(nav, 10, 0);
    lv_obj_set_style_pad_ver(nav, 0, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_back_btn = icon_button(nav, LV_SYMBOL_LEFT);

    lv_obj_t *title = lv_label_create(nav);
    lv_label_set_text(title, "AI 聊天");
    set_font(title);
    lv_obj_set_style_text_color(title, lv_color_hex(0x58A6FF), 0);
    lv_obj_set_style_margin_left(title, 12, 0);

    /* 情绪徽章：显示 AI 当前感知到的情绪 */
    s_emo_badge = lv_obj_create(nav);
    lv_obj_set_size(s_emo_badge, LV_SIZE_CONTENT, 26);
    lv_obj_set_style_bg_color(s_emo_badge, lv_color_hex(0x78909C), 0);
    lv_obj_set_style_bg_opa(s_emo_badge, LV_OPA_30, 0);
    lv_obj_set_style_radius(s_emo_badge, 13, 0);
    lv_obj_set_style_border_width(s_emo_badge, 1, 0);
    lv_obj_set_style_border_color(s_emo_badge, lv_color_hex(0x78909C), 0);
    lv_obj_set_style_pad_hor(s_emo_badge, 8, 0);
    lv_obj_set_style_margin_left(s_emo_badge, 8, 0);
    lv_obj_add_flag(s_emo_badge, LV_OBJ_FLAG_HIDDEN);
    s_emo_label = lv_label_create(s_emo_badge);
    lv_label_set_text(s_emo_label, "平静 92%");
    set_font(s_emo_label);
    lv_obj_set_style_text_color(s_emo_label, lv_color_white(), 0);
    lv_obj_center(s_emo_label);

    /* 占位弹簧：把清空按钮推到右边 */
    lv_obj_t *nav_spacer = lv_obj_create(nav);
    lv_obj_set_size(nav_spacer, 1, 1);
    lv_obj_set_flex_grow(nav_spacer, 1);
    lv_obj_set_style_bg_opa(nav_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav_spacer, 0, 0);

    s_clear_btn = icon_button(nav, LV_SYMBOL_TRASH);
    lv_obj_add_event_cb(s_clear_btn, on_clear, LV_EVENT_CLICKED, NULL);

    s_messages = lv_obj_create(s_screen);
    lv_obj_set_width(s_messages, LV_PCT(100));
    lv_obj_set_flex_grow(s_messages, 1);
    lv_obj_set_style_bg_opa(s_messages, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_messages, 0, 0);
    lv_obj_set_style_radius(s_messages, 0, 0);
    lv_obj_set_style_pad_hor(s_messages, 16, 0);
    lv_obj_set_style_pad_ver(s_messages, 12, 0);
    lv_obj_set_flex_flow(s_messages, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_messages, 10, 0);
    lv_obj_set_scroll_dir(s_messages, LV_DIR_VER);

    append_message("Assistant", "Send a message to include the latest local emotion.", 0x8B949E);

    lv_obj_t *input_bar = lv_obj_create(s_screen);
    lv_obj_set_size(input_bar, LV_PCT(100), 68);
    lv_obj_set_style_bg_color(input_bar, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_width(input_bar, 0, 0);
    lv_obj_set_style_border_side(input_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(input_bar, 1, 0);
    lv_obj_set_style_border_color(input_bar, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_radius(input_bar, 0, 0);
    lv_obj_set_style_pad_hor(input_bar, 10, 0);
    lv_obj_set_style_pad_ver(input_bar, 6, 0);
    lv_obj_set_flex_flow(input_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_input = lv_textarea_create(input_bar);
    lv_obj_set_height(s_input, 52);
    lv_obj_set_flex_grow(s_input, 1);
    lv_textarea_set_one_line(s_input, true);
    lv_textarea_set_placeholder_text(s_input, "Type a message...");
    set_font(s_input);
    lv_obj_set_style_bg_color(s_input, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_border_color(s_input, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_text_color(s_input, lv_color_hex(0xC9D1D9), 0);
    lv_obj_add_event_cb(s_input, on_input, LV_EVENT_FOCUSED, NULL);

    s_send_btn = icon_button(input_bar, LV_SYMBOL_RIGHT);
    lv_obj_set_style_margin_left(s_send_btn, 8, 0);
    lv_obj_add_event_cb(s_send_btn, on_send, LV_EVENT_CLICKED, NULL);

    s_voice_btn = icon_button(input_bar, LV_SYMBOL_AUDIO);
    lv_obj_set_style_margin_left(s_voice_btn, 8, 0);
    lv_obj_add_event_cb(s_voice_btn, on_voice_start, LV_EVENT_CLICKED, NULL);

    s_status = lv_label_create(s_screen);
    lv_label_set_text(s_status, "Ready");
    set_font(s_status);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x8B949E), 0);
    lv_obj_set_style_pad_left(s_status, 16, 0);
    lv_obj_set_style_pad_top(s_status, 2, 0);
    lv_obj_set_style_pad_bottom(s_status, 2, 0);

    s_keyboard = lv_keyboard_create(s_screen);
    lv_obj_set_width(s_keyboard, LV_PCT(100));
    lv_obj_set_height(s_keyboard, 240);
    lv_keyboard_set_textarea(s_keyboard, s_input);
    lv_obj_set_style_border_side(s_keyboard, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_keyboard, 1, 0);
    lv_obj_set_style_border_color(s_keyboard, lv_color_hex(0x30363D), 0);
    lv_obj_add_event_cb(s_keyboard, on_keyboard, LV_EVENT_READY, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    return s_screen;
}

void ui_chat_set_back_callback(lv_event_cb_t cb)
{
    if (s_back_btn) lv_obj_add_event_cb(s_back_btn, cb, LV_EVENT_CLICKED, NULL);
}

void ui_chat_set_voice_start_callback(ui_chat_voice_start_cb_t cb)
{
    s_voice_start_cb = cb;
}

/* 从摄像头回调更新情绪徽章（实时反映检测到的情绪） */
void ui_chat_update_emotion(int cls, float conf)
{
    if (!s_emo_badge || !s_emo_label) return;
    if (cls < 0 || cls > 6) { lv_obj_add_flag(s_emo_badge, LV_OBJ_FLAG_HIDDEN); return; }

    int pct = (int)(conf * 100.0f + 0.5f);
    if (pct > 100) pct = 100;

    lv_label_set_text_fmt(s_emo_label, "%s %d%%", EMO_NAMES[cls], pct);
    lv_obj_set_style_bg_color(s_emo_badge, lv_color_hex(EMO_COLORS[cls]), 0);
    lv_obj_clear_flag(s_emo_badge, LV_OBJ_FLAG_HIDDEN);
}

void ui_chat_voice_append_user(const char *text)
{
    if (!text || !text[0]) return;
    if (lvgl_port_lock(-1)) {
        append_message("You (voice)", text, 0x58A6FF);
        lvgl_port_unlock();
    }
}

void ui_chat_voice_append_assistant(const char *text)
{
    if (!text || !text[0]) return;
    if (lvgl_port_lock(-1)) {
        append_message("XiaoZhi", text, 0xC9D1D9);
        lvgl_port_unlock();
    }
}

void ui_chat_voice_set_status(const char *status)
{
    if (!status) return;
    if (lvgl_port_lock(-1)) {
        if (s_status) lv_label_set_text(s_status, status);
        lvgl_port_unlock();
    }
}

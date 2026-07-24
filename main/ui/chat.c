#include "chat.h"
#include "llm_client.h"
#include "esp_lvgl_port.h"
#include "ui_font_zh_22.h"

LV_FONT_DECLARE(lv_font_montserrat_14);

#define CHAT_MAX_MESSAGES 40

static lv_obj_t *s_screen;
static lv_obj_t *s_messages;
static lv_obj_t *s_input;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_send_btn;
static lv_obj_t *s_clear_btn;
static lv_obj_t *s_status;
static lv_obj_t *s_back_btn;

static void set_font(lv_obj_t *obj)
{
    lv_obj_set_style_text_font(obj, &ui_font_zh_22, 0);
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

    while (lv_obj_get_child_count(s_messages) >= CHAT_MAX_MESSAGES) {
        lv_obj_delete(lv_obj_get_child(s_messages, 0));
    }

    lv_obj_t *message = lv_label_create(s_messages);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(message, LV_PCT(100));
    lv_label_set_text_fmt(message, "%s: %s", speaker, text);
    set_font(message);
    lv_obj_set_style_text_color(message, lv_color_hex(color), 0);
    lv_obj_scroll_to_view(message, LV_ANIM_OFF);
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
    send_message();
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
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
}

static lv_obj_t *icon_button(lv_obj_t *parent, const char *icon)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, 40, 36);
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
    lv_obj_set_size(nav, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(nav, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_width(nav, 0, 0);
    lv_obj_set_style_radius(nav, 0, 0);
    lv_obj_set_style_pad_hor(nav, 8, 0);
    lv_obj_set_style_pad_ver(nav, 0, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_back_btn = icon_button(nav, LV_SYMBOL_LEFT);

    lv_obj_t *title = lv_label_create(nav);
    lv_label_set_text(title, "AI Chat");
    set_font(title);
    lv_obj_set_style_text_color(title, lv_color_hex(0x58A6FF), 0);
    lv_obj_set_style_margin_left(title, 12, 0);

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
    lv_obj_set_size(input_bar, LV_PCT(100), 58);
    lv_obj_set_style_bg_color(input_bar, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_width(input_bar, 0, 0);
    lv_obj_set_style_radius(input_bar, 0, 0);
    lv_obj_set_style_pad_hor(input_bar, 10, 0);
    lv_obj_set_style_pad_ver(input_bar, 8, 0);
    lv_obj_set_flex_flow(input_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_input = lv_textarea_create(input_bar);
    lv_obj_set_height(s_input, 40);
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

    s_status = lv_label_create(s_screen);
    lv_label_set_text(s_status, "Ready");
    set_font(s_status);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x8B949E), 0);
    lv_obj_set_style_pad_left(s_status, 16, 0);
    lv_obj_set_style_pad_top(s_status, 4, 0);
    lv_obj_set_style_pad_bottom(s_status, 4, 0);

    s_keyboard = lv_keyboard_create(s_screen);
    lv_obj_set_width(s_keyboard, LV_PCT(100));
    lv_obj_set_height(s_keyboard, 190);
    lv_keyboard_set_textarea(s_keyboard, s_input);
    lv_obj_add_event_cb(s_keyboard, on_keyboard, LV_EVENT_READY, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    return s_screen;
}

void ui_chat_set_back_callback(lv_event_cb_t cb)
{
    if (s_back_btn) lv_obj_add_event_cb(s_back_btn, cb, LV_EVENT_CLICKED, NULL);
}
